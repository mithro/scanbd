/*
 * $Id: sane.c 241 2017-04-19 07:53:25Z wimalopaan $
 *
 *  scanbd - KMUX scanner button daemon
 *
 *  Copyright (C) 2008 - 2017 Wilhelm Meier (wilhelm.wm.meier@googlemail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "scanbd.h"
#include "scanbd_dbus.h"

#include <libusb-1.0/libusb.h>

#define CANCEL_TEST

// all programm-global sane functions use this mutex to avoid races
#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
// this is non-portable
static pthread_mutex_t sane_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t sane_mutex;
#endif

static pthread_cond_t  sane_cv    = PTHREAD_COND_INITIALIZER;

// the following locking strategie must be obeyed:
// 1) lock the sane_mutex
// 2) lock the device specific mutex
// in this order to avoid deadlocks
// holding more than these two locks is not intended

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
void sane_init_mutex()
{
    slog(SLOG_INFO, "sane_init_mutex");
    pthread_mutexattr_t mutexattr;
    if (pthread_mutexattr_init(&mutexattr) < 0) {
        slog(SLOG_ERROR, "Can't initialize mutex attr");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE) < 0) {
        slog(SLOG_ERROR, "Can't set mutex attr");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&sane_mutex, &mutexattr) < 0) {
        slog(SLOG_ERROR, "Can't init mutex");
        exit(EXIT_FAILURE);
    }
}
#endif

struct sane_opt_value {    
    unsigned long num_value; // before-value or after-value or actual-value (BOOL|INT|FIXED)
    struct {                 // (STRING)
        char*     str;       // actual-value
        regex_t*  reg;       // before-regex or after-regex
    } str_value;
};
typedef struct sane_opt_value sane_opt_value_t;

struct sane_dev_option {
    int number;                  // the option-number of the device-option
    sane_opt_value_t from_value; // the before-value of the option
    sane_opt_value_t to_value;   // the after-value of the option (to
    // fire the trigger)
    sane_opt_value_t value;      // the option value (from the last
    // polling cycle)
    const char* script;          // the found (matched) script to be called if
    // the option-valued changes
    const char* action_name;	 // the name of this action as
    // specified in the config file
};
typedef struct sane_dev_option sane_dev_option_t;

struct sane_dev_function {
    int number;			 // the option-number of the
    // device-option
    const char* env;		 // the name of the environment-var to
    // pass to option value in
};
typedef struct sane_dev_function sane_dev_function_t;

// each polling thread is represented by struct sane_thread
// there is no locking, since this is "thread private data"
struct sane_thread {
    pthread_t tid;                   // the thread-id of the polling
    // thread
    pthread_mutex_t mutex;	     // mutex for this data-structure
    pthread_cond_t cv;		     // cv for this data-structure
    bool triggered;		     // a rule for this device has fired (triggered == true)
    int  triggered_option;           // the action number which triggered
    const SANE_Device* dev;          // the device
    int num_of_options;	             // the number of all options for
    // this device
    SANE_Handle h;                   // the handle of the opened device
    sane_dev_option_t *opts;         // the list of matched actions
    // for this device
    int num_of_options_with_scripts; // the number of elements in the
    // above list
    sane_dev_function_t *functions;  // the list of matched functions
    // for this device
    int num_of_options_with_functions;// the number of elements in the
    // above list
};
typedef struct sane_thread sane_thread_t;

// the list of all polling threads
static sane_thread_t* sane_poll_threads = NULL;

// the list of all devices locally connected to our system
static const SANE_Device** sane_device_list = NULL;

// the number of devices = the number of polling threads
static int num_devices = 0;

void get_sane_devices(void) {
    // detect all the scanners we have
    slog(SLOG_INFO, "Scanning for local-only devices" );

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    SANE_Status sane_status = SANE_STATUS_INVAL;
    sane_device_list = NULL;
    num_devices = 0;
    if ((sane_status = sane_get_devices(&sane_device_list, SANE_TRUE)) != SANE_STATUS_GOOD) {
        slog(SLOG_WARN, "Can't get the sane device list");
    }
    const SANE_Device** dev = sane_device_list;
    if (dev == NULL) {
        slog(SLOG_DEBUG, "device list null");
        goto cleanup;
    }
    while(*dev != NULL) {
        slog(SLOG_DEBUG, "found device: %s %s %s %s",
             (*dev)->name, (*dev)->vendor, (*dev)->model, (*dev)->type);
        num_devices += 1;
        dev++;
    }
    if (pthread_cond_broadcast(&sane_cv)) {
        slog(SLOG_ERROR, "pthread_cond_broadcast: %s", strerror(errno));
    }
cleanup:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
        return;
    }
}	

// simple hash function for C-strings
static unsigned long hash(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

static void sane_option_value_init(sane_opt_value_t* v) {
    v->num_value = 0;
    v->str_value.str = NULL;
    v->str_value.reg = NULL;
}

static void sane_option_value_free(sane_opt_value_t* v) {
    if (v->str_value.str != NULL) {
        free((void*)v->str_value.str);
        v->str_value.str = NULL;
    }
    if (v->str_value.reg != NULL) {
        regfree(v->str_value.reg);
        free(v->str_value.reg);
        v->str_value.reg = NULL;
    }
}

static sane_opt_value_t get_sane_option_value(SANE_Handle* h, int index) {
    slog(SLOG_DEBUG, "get_sane_option_value");
    // get the value of option with index of the device (opened) with
    // handle h
    // if option can't be found or other catastrophy happens, the
    // value 0 gets returned
#if ((__STDC_VERSION__  - 0) < 201112L) || ((__GNUC__ - 0) < 5)
    sane_opt_value_t res;
#else
    sane_opt_value_t res = {};
#endif

    sane_option_value_init(&res);

    const SANE_Option_Descriptor* odesc = NULL;
    if ((odesc = sane_get_option_descriptor(h, index)) == NULL) {
        return res;
    }
    if ((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
            (odesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
        unsigned long int value = 0;
        if ((unsigned int)odesc->size <= sizeof(long int)) {
            //if we can store it in an long int
            SANE_Status status = SANE_STATUS_INVAL;
            if ((status = sane_control_option(h, index, SANE_ACTION_GET_VALUE,
                                              &value, NULL)) != SANE_STATUS_GOOD) {
                slog(SLOG_WARN, "Can't read value of %s: %s",
                     odesc->name, sane_strstatus(status));
                return res;
            }
            res.num_value = value;
            return res;
        }
        else {
            // shouldn't happen
            slog(SLOG_WARN, "Value of %s, sane-type %d too big", odesc->name, odesc->type);
            return res;
        }
    }
    else if (odesc->type == SANE_TYPE_STRING) {
        res.str_value.str = calloc(odesc->size + 1, sizeof(char));
        assert(res.str_value.str != NULL);
        SANE_Status status = SANE_STATUS_INVAL;
        if ((status = sane_control_option(h, index, SANE_ACTION_GET_VALUE,
                                          res.str_value.str, NULL)) != SANE_STATUS_GOOD) {
            slog(SLOG_WARN, "Can't read value of %s: %s", odesc->name, sane_strstatus(status));
            return res;
        }
        res.str_value.str[odesc->size] = '\0';
        size_t slen = strlen(res.str_value.str);
        res.num_value = hash(res.str_value.str);

        slog(SLOG_INFO, "Value of %s as string (len %d, hash %d): %s",
             odesc->name, slen, res.num_value, res.str_value.str);
        return res;
    }
    else {
        slog(SLOG_WARN, "Can't read option %s of type %d", odesc->name, odesc->type);
    }
    return res;
}


// cleanup handler for sane_poll
static void sane_thread_cleanup_mutex(void* arg) {
    assert(arg != NULL);
    slog(SLOG_DEBUG, "sane_thread_cleanup_mutex");
    pthread_mutex_t* mutex = (pthread_mutex_t*)arg;
    if (pthread_mutex_unlock(mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
    }
}

// cleanup handler for sane_poll
static void sane_thread_cleanup_value(void* arg) {
    assert(arg != NULL);
    slog(SLOG_DEBUG, "sane_thread_cleanup_value");
    sane_opt_value_t* v = (sane_opt_value_t*)arg;
    sane_option_value_free(v);
}


// this function can only be used in the critical region of *st
static void sane_find_matching_functions(sane_thread_t* st, cfg_t* sec) {
    // TODO: use of recursive mutex???
    slog(SLOG_DEBUG, "sane_find_matching_functions");
    const char* title = cfg_title(sec);
    if (title == NULL) {
        title = SCANBD_NULL_STRING;
    }
    int functions = cfg_size(sec, C_FUNCTION);
    if (functions <= 0) {
        slog(SLOG_INFO, "no matching functions in section %s", title);
        return;
    }
    
    slog(SLOG_INFO, "found %d functions in section %s", functions, title);
    // iterate over all global functions
    for(int i = 0; i < functions; i += 1) {
        // get the function from the config file
        cfg_t* function_i = cfg_getnsec(sec, C_FUNCTION, i);
        assert(function_i != NULL);

        // get the filter-regex from the config-file
        const char* opt_regex = cfg_getstr(function_i, C_FILTER);
        assert(opt_regex != NULL);

        const char* title = cfg_title(function_i);
        if (title == NULL) {
            title = "(none)";
        }
        // compile the filter-regex
        slog(SLOG_DEBUG, "checking function %s with filter: %s",
             title, opt_regex);
        regex_t creg;
        int ret = regcomp(&creg, opt_regex, REG_EXTENDED | REG_NOSUB);
        if (ret < 0) {
            char err_text[1024];
            regerror(ret, &creg, err_text, 1024);
            slog(SLOG_WARN, "Can't compile regex: %s : %s", opt_regex, err_text);
            continue;
        }
        // look for matching option-names
        for(int opt = 1; opt < st->num_of_options; opt += 1) {
            const SANE_Option_Descriptor* odesc = NULL;
            if ((odesc = sane_get_option_descriptor(st->h, opt)) == NULL) {
                // no valid option-descriptor available
                // skip it
                slog(SLOG_INFO, "option[%d] has no valid descriptor", opt);
                continue;
            }
            assert(odesc);
            if (!SANE_OPTION_IS_ACTIVE(odesc->cap)) {
                slog(SLOG_INFO, "option[%d] is not active", opt);
                continue;
            }
            // option is active
            // only use active (user controllable) options
            if (odesc->name == NULL) {
                // we need a valid option-name
                slog(SLOG_INFO, "option[%d] has no name", opt);
                continue;
            }
            assert(odesc->name);
            if (!((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                  (odesc->type == SANE_TYPE_FIXED)|| (odesc->type == SANE_TYPE_STRING) ||
                  (odesc->type == SANE_TYPE_BUTTON))) {
                slog(SLOG_WARN, "option[%d] %s for device %s not of "
                     "type BOOL|INT|FIXED|STRING|BUTTON. Skipping",
                     opt, odesc->name, st->dev->name);
                continue;
            }
            slog(SLOG_INFO, "found active option[%d] %s (type: %d) for device %s",
                 opt, odesc->name, odesc->type, st->dev->name);
            // regex compare with the filter
            if (regexec(&creg, odesc->name, 0, NULL, 0) != 0) {
                // no match
                continue;
            }
            // match

            // now get the script
            const char* env = cfg_getstr(function_i, C_ENV);
            assert(env != NULL);
            slog(SLOG_INFO, "installing function %s for %s, option[%d]: %s as env: %s",
                 title, st->dev->name, opt, odesc->name, env);

            // looking for option already present in the
            // array
            int n = 0;
            for(n = 0; n < st->num_of_options_with_functions; n += 1) {
                if (st->functions[n].number == opt) {
                    slog(SLOG_WARN, "function %s overrides function of option[%d]",
                         title, n);
                    // break out with n == index_of_found_option
                    break;
                }
            }
            // 0 <= n < st->num_of_options_with_scripts:
            // we found it
            // n == st->num_of_options_with_scripts:
            // not found => new

            st->functions[n].number = opt;
            st->functions[n].env = env;

            if (n == st->num_of_options_with_functions) {
                // not found in the list
                // we have a new option to be polled
                st->num_of_options_with_functions += 1;
            }
        } // foreach option
        // this compiled regex isn't used anymore
        regfree(&creg);
    } // foreach action
}

// this function can only be used in the critical region of *st
static void sane_find_matching_options(sane_thread_t* st, cfg_t* sec) {
    slog(SLOG_DEBUG, "sane_find_matching_options");
    const char* title = cfg_title(sec);
    if (title == NULL) {
        title = SCANBD_NULL_STRING;
    }
    // TODO: use of recursive mutex???
    int actions = cfg_size(sec, C_ACTION);
    if (actions <= 0) {
        slog(SLOG_INFO, "no matching actions in section %s",  title);
        return;
    }
    
    slog(SLOG_INFO, "found %d actions in section %s", actions, title);

    // iterate over all global actions
    for(int i = 0; i < actions; i += 1) {
        // get the action from the config file
        cfg_t* action_i = cfg_getnsec(sec, C_ACTION, i);
        assert(action_i != NULL);

        // get the filter-regex from the config-file
        const char* opt_regex = cfg_getstr(action_i, C_FILTER);
        assert(opt_regex != NULL);

        const char* title = cfg_title(action_i);
        if (title == NULL) {
            title = "(none)";
        }
        // compile the filter-regex
        slog(SLOG_DEBUG, "checking action %s with filter: %s",
             title, opt_regex);
        regex_t creg;
        int ret = regcomp(&creg, opt_regex, REG_EXTENDED | REG_NOSUB);
        if (ret < 0) {
            char err_text[1024];
            regerror(ret, &creg, err_text, 1024);
            slog(SLOG_WARN, "Can't compile regex: %s : %s", opt_regex, err_text);
            continue;
        }
        // look for matching option-names
        for(int opt = 1; opt < st->num_of_options; opt += 1) {
            const SANE_Option_Descriptor* odesc = NULL;
            if ((odesc = sane_get_option_descriptor(st->h, opt)) == NULL) {
                // no valid option-descriptor available
                // skip it
                continue;
            }
            assert(odesc);
            if (!SANE_OPTION_IS_ACTIVE(odesc->cap)) {
                continue;
            }
            // option is active
            // only use active (user controllable) options
            if (odesc->name == NULL) {
                // we need a valid option-name
                continue;
            }
            assert(odesc->name);
            if (!((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                  (odesc->type == SANE_TYPE_FIXED)|| (odesc->type == SANE_TYPE_STRING) ||
                  (odesc->type == SANE_TYPE_BUTTON))) {
                slog(SLOG_WARN, "option[%d] %s for device %s not of "
                     "type BOOL|INT|FIXED|STRING|BUTTON. Skipping",
                     opt, odesc->name, st->dev->name);
                continue;
            }
            slog(SLOG_INFO, "found active option[%d] %s (type: %d) for device %s",
                 opt, odesc->name, odesc->type, st->dev->name);
            // regex compare with the filter
            if (regexec(&creg, odesc->name, 0, NULL, 0) != 0) {
                // no match
                continue;
            }
            // match

            // now get the script from the action

            const char* script = cfg_getstr(action_i, C_SCRIPT);

            if (!script || (strlen(script) == 0)) {
                script = SCANBD_NULL_STRING;
            }       

            assert(script != NULL);
            slog(SLOG_INFO, "installing action %s (%d) for %s, option[%d]: %s as: %s",
                 title, st->num_of_options_with_scripts, st->dev->name, opt, odesc->name, script);

            // get pointer to global section of config

            cfg_t* cfg_sec_global = NULL;
            cfg_sec_global = cfg_getsec(cfg, C_GLOBAL);
            assert(cfg_sec_global);

            bool multiple_actions = cfg_getbool(cfg_sec_global, C_MULTIPLE_ACTIONS);
            slog(SLOG_INFO, "multiple actions allowed");

            // looking for option already present in the
            // array
            int n = 0;
            for(n = 0; n < st->num_of_options_with_scripts; n += 1) {
                if (st->opts[n].number == opt) {
                    if (!multiple_actions) {
                        slog(SLOG_WARN, "action %s overrides script %s of option[%d] with %s",
                             title, st->opts[n].script, opt, script);
                        // break out with n == index_of_found_option
                        break;
                    }
                    else {
                        if (n < st->num_of_options) {
                            n = st->num_of_options_with_scripts;
                            slog(SLOG_INFO, "adding additional action %s (%d) for option[%d] with %s",
                                 title, n, opt, script);
                            break;
                        }
                        else {
                            slog(SLOG_INFO, "can't add additional action %s for option[%d] with %s",
                                 title, opt, script);
                            break;
                        }
                    }
                }
            }
            // 0 <= n < st->num_of_options_with_scripts:
            // we found it (override now)
            // n == st->num_of_options_with_scripts:
            // not found => new

            if (n == st->num_of_options) {
                continue; // no space left in array
            }
            st->opts[n].number = opt;
            st->opts[n].action_name = title;
            st->opts[n].script = script;
            sane_option_value_free(&st->opts[n].from_value);
            sane_option_value_free(&st->opts[n].to_value);
            sane_option_value_free(&st->opts[n].value);

            if ((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                    (odesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
                // numerical option
                cfg_t* num_trigger = cfg_getsec(action_i, C_NUMERICAL_TRIGGER);
                assert(num_trigger);
                st->opts[n].from_value.num_value = cfg_getint(num_trigger,
                                                              C_FROM_VALUE);
                st->opts[n].to_value.num_value = cfg_getint(num_trigger, C_TO_VALUE);

                st->opts[n].value = get_sane_option_value(st->h, opt);

                // same struct-vs-value bug as the poll log: print .num_value
                // (this branch is BOOL | INT | FIXED), not the whole struct.
                slog(SLOG_INFO, "Initial value of option %s is %lu", odesc->name,
                     st->opts[n].value.num_value);
            } // type BOOL | INT || FIXED
            else if (odesc->type == SANE_TYPE_STRING) {
                bool valid = true;
                // string option
                cfg_t* str_trigger = cfg_getsec(action_i, C_STRING_TRIGGER);
                assert(str_trigger);

                st->opts[n].from_value.str_value.str =
                        strdup(cfg_getstr(str_trigger,
                                          C_FROM_VALUE));
                st->opts[n].from_value.str_value.reg = malloc(sizeof(regex_t));
                int ret = 0;
                ret = regcomp(st->opts[n].from_value.str_value.reg,
                              st->opts[n].from_value.str_value.str,
                              REG_EXTENDED | REG_NOSUB);
                if (ret < 0) {
                    char err_text[1024];
                    regerror(ret, &creg, err_text, 1024);
                    slog(SLOG_WARN, "Can't compile regex: %s : %s",
                         st->opts[n].from_value.str_value.str, err_text);
                    valid = false;;
                }
                st->opts[n].to_value.str_value.str =
                        strdup(cfg_getstr(str_trigger,
                                          C_TO_VALUE));
                st->opts[n].to_value.str_value.reg = malloc(sizeof(regex_t));
                ret = regcomp(st->opts[n].to_value.str_value.reg,
                              st->opts[n].to_value.str_value.str,
                              REG_EXTENDED | REG_NOSUB);
                if (ret < 0) {
                    char err_text[1024];
                    regerror(ret, &creg, err_text, 1024);
                    slog(SLOG_WARN, "Can't compile regex: %s : %s",
                         st->opts[n].to_value.str_value.str, err_text);
                    valid = false;;
                }

                st->opts[n].value = get_sane_option_value(st->h, opt);

                if (!valid) {
                    sane_option_value_free(&st->opts[n].from_value);
                    sane_option_value_free(&st->opts[n].to_value);
                    sane_option_value_free(&st->opts[n].value);
                    continue;
                }
            } // type STRING
            else {
                assert(false); // should not happen
            }
            if (n == st->num_of_options_with_scripts) {
                // not found in the list
                // we have a new option to be polled
                st->num_of_options_with_scripts += 1;
            }
        } // foreach option
        // this compiled regex isn't used anymore
        regfree(&creg);
    } // foreach action
}


// Force a refresh of the backend's cached button/event state, once per poll
// pass, BEFORE the monitored options are read.
//
// Root cause (welland patch): the SANE pixma backend (Canon PIXMA / CanoScan,
// e.g. the LiDE 400) only re-reads the scanner's physical button/event state
// from the device as a side effect of an *ACTION* on its SANE_TYPE_BUTTON
// option "button-update" -- and specifically ONLY on SANE_ACTION_SET_VALUE
// (backend/pixma/pixma.c: case opt_button_update refreshes on SET_VALUE and
// returns SANE_STATUS_INVAL on GET_VALUE). Reading button-1/button-2/target/
// original with GET_VALUE returns a STALE cached word until that refresh runs.
//
// scanbd's poll loop otherwise only issues GET_VALUE, and only on the handful
// of options that carry a configured action/function. It therefore never
// drives the pixma refresh, so `target`/`button-*` never change and no press
// is ever seen -- even though `scanimage -A`, which walks and reads every
// option (including the low-index button options that trigger the internal
// refresh) in one pass, reports presses correctly. Trying to work around this
// purely in scanbd.conf (a `function button-update {}` read first) does NOT
// help, because scanbd GETs it -- a no-op that returns INVAL -- instead of
// SETting it.
//
// Fix: at the top of every poll pass, SET_VALUE the "button-update" option
// (matched by name among the active, settable SANE_TYPE_BUTTON options). That
// is the backend-documented "Update button state" trigger; it refreshes the
// cached button/target words on the same open handle, so the subsequent
// GET_VALUE reads observe the real press. Backends without a "button-update"
// option are unaffected -- the option is simply not found and nothing is set;
// no other button option is touched, so no unrelated action is fired.
// --- welland3: decode WHICH pixma button was pressed --------------------
//
// The pressed panel button is encoded in the read-only "target" option. For
// the CanoScan LiDE 300/400 the SANE pixma backend
// (backend/pixma/pixma_mp150.c handle_interrupt(), the LIDE400_PID/LIDE300_PID
// branches) packs the raw interrupt byte buf[0x13] into the event word as
//   s->events = PIXMA_EV_BUTTON1 | (buf[0x13] & 0x0f)   (BUTTON2 for 0x06),
// and pixma.h's GET_EV_TARGET(ev) = ev & 0x0f, so update_button_state() stores
// OVAL(target) = buf[0x13]:
//   1 = copy   2 = auto-scan   3 = send   5 = start-PDF   6 = finish-PDF
// (finish-PDF is the sole button-2 key; all others are button-1). original,
// scan-resolution and document-type stay 0 for this model.
//
// button-1/button-2/target/original are soft_detect-only (READ-ONLY) and are
// written ONLY on a button-1/2 transition -- which is also the only time pixma
// sets SANE_INFO_RELOAD_OPTIONS. Off a transition they are stale/uninitialised
// (the -363474928 garbage), so we read and trust them ONLY when the
// button-update SET reports RELOAD.
//
// pixma latches button-1/2 at 1 until the device is reopened, so a long-lived
// scanbd would otherwise only ever decode the FIRST press. After each decoded
// event we reopen the device (scanbd already close/reopens around every
// action, so this is a supported reset) to restore button-1/2 to their default
// 0, so the next distinct press is a fresh transition that updates target.

static int sane_find_option_by_name(sane_thread_t* st, const char* name) {
    for (int opt = 1; opt < st->num_of_options; opt += 1) {
        const SANE_Option_Descriptor* d = sane_get_option_descriptor(st->h, opt);
        if (d != NULL && d->name != NULL && strcmp(d->name, name) == 0) {
            return opt;
        }
    }
    return -1;
}

// Read a scalar (INT/BOOL/BUTTON) option by index into *out. Returns true on a
// good read; leaves *out untouched otherwise.
static bool sane_read_word_option(sane_thread_t* st, int opt, long* out) {
    if (opt < 0) {
        return false;
    }
    const SANE_Option_Descriptor* d = sane_get_option_descriptor(st->h, opt);
    if (d == NULL || !SANE_OPTION_IS_ACTIVE(d->cap)) {
        return false;
    }
    SANE_Word w = 0;
    if (sane_control_option(st->h, opt, SANE_ACTION_GET_VALUE, &w, NULL) != SANE_STATUS_GOOD) {
        return false;
    }
    *out = (long)(SANE_Int)w;
    return true;
}

// pixma LiDE 300/400 button code (OVAL(target)) -> panel button name.
static const char* pixma_lide_button_name(long target) {
    switch (target) {
        case 1: return "copy";
        case 2: return "auto-scan";
        case 3: return "send";
        case 5: return "start-pdf";
        case 6: return "finish-pdf";   // the button-2 key (stop / finish PDF)
        default: return "unknown";
    }
}

static void sane_refresh_button_state(sane_thread_t* st) {
    assert(st != NULL);

    int bu = sane_find_option_by_name(st, "button-update");
    if (bu < 0) {
        return;   // not a pixma-style backend; nothing to refresh
    }
    const SANE_Option_Descriptor* bud = sane_get_option_descriptor(st->h, bu);
    if (bud == NULL || bud->type != SANE_TYPE_BUTTON ||
        !SANE_OPTION_IS_ACTIVE(bud->cap) || !SANE_OPTION_IS_SETTABLE(bud->cap)) {
        slog(SLOG_DEBUG, "button-update option %d not active/settable, skipping", bu);
        return;
    }

    // welland1: SET button-update refreshes the backend's cached button/event
    // state on this handle. welland3: capture 'info' to learn whether a NEW
    // button transition occurred this poll.
    SANE_Word dummy = 0;
    SANE_Int info = 0;
    SANE_Status status = sane_control_option(st->h, bu, SANE_ACTION_SET_VALUE,
                                             &dummy, &info);
    if (status != SANE_STATUS_GOOD) {
        slog(SLOG_DEBUG, "button-update refresh (opt %d) returned: %s",
             bu, sane_strstatus(status));
        return;
    }

    if (!(info & SANE_INFO_RELOAD_OPTIONS)) {
        // No new button transition this poll: the button/target/original words
        // are stale/uninitialised, so we deliberately do NOT read or report
        // them (this is what stops the -363474928 garbage being logged).
        slog(SLOG_DEBUG, "button-update refresh (opt %d): no new event this poll", bu);
        return;
    }

    // A genuine button transition happened: every button-related option is
    // freshly valid right now. Read them all and decode.
    long target = -1, original = -1, button1 = -1, button2 = -1;
    long scanres = -1, doctype = -1;
    sane_read_word_option(st, sane_find_option_by_name(st, "target"),          &target);
    sane_read_word_option(st, sane_find_option_by_name(st, "original"),        &original);
    sane_read_word_option(st, sane_find_option_by_name(st, "button-1"),        &button1);
    sane_read_word_option(st, sane_find_option_by_name(st, "button-2"),        &button2);
    sane_read_word_option(st, sane_find_option_by_name(st, "scan-resolution"), &scanres);
    sane_read_word_option(st, sane_find_option_by_name(st, "document-type"),   &doctype);

    // SLOG_ERROR so it is visible without -d7 (scanbd logs "trigger action" at
    // ERROR too). One rich line per real press -> a single press round reveals
    // the discriminator.
    slog(SLOG_ERROR,
         "pixma button EVENT on %s: decoded=%s | target=%ld original=%ld "
         "button-1=%ld button-2=%ld scan-resolution=%ld document-type=%ld",
         st->dev->name, pixma_lide_button_name(target), target, original,
         button1, button2, scanres, doctype);

    // Reset the pixma button-1/2 latch by reopening, so the NEXT distinct press
    // is a fresh transition that updates 'target'. Mirrors scanbd's own
    // close/reopen around actions.
    sane_close(st->h);
    st->h = NULL;
    for (int attempt = 1; ; attempt += 1) {
        status = sane_open(st->dev->name, &st->h);
        if (status == SANE_STATUS_GOOD) {
            slog(SLOG_DEBUG, "reopened %s to reset pixma button latch", st->dev->name);
            break;
        }
        slog(SLOG_ERROR, "reopen of %s after button event failed (attempt %d): %s",
             st->dev->name, attempt, sane_strstatus(status));
        if (attempt >= 5) {
            slog(SLOG_ERROR, "giving up reopening %s; abandoning its polling thread",
                 st->dev->name);
            pthread_exit(NULL);
        }
    }
}

// ======================================================================
// welland4: event-driven front-button detection via the raw USB interrupt
// endpoint (pixma / CanoScan LiDE), replacing the SANE-option poll.
//
// The SANE pixma option path (welland1-3) can only observe a button on a
// button-1/2 transition and latches until reopen. The scanner ALSO emits the
// same event, cleanly and instantly, on a USB interrupt-IN endpoint on
// interface 0: a 32-byte packet where buf[4]==0x01 flags a press and buf[19]
// carries the SAME per-button code as the SANE `target` option
//   1=copy 2=auto-scan 3=send 5=start-pdf 6=finish-pdf
// (measured on rpi5-scanner AND confirmed against pixma_mp150.c
//  handle_interrupt(): buf[0x13] is exactly byte 19). So for such a device we
// BLOCK on that endpoint instead of polling SANE -- instant, no polling, all 5
// buttons distinguishable.
//
// Device sharing (the crux): a libusb claim of interface 0 and a pixma/saned
// scan cannot coexist. So we hold interface 0 while waiting, and on a decoded
// event RELEASE it (close the libusb handle) BEFORE running the action -- so
// the action's own scan (direct pixma or via saned) can open the scanner --
// then RECLAIM interface 0 afterwards. This mirrors the proven PoC and
// scanbd's own close/reopen around actions.
//
// Gated hard: only a device whose SANE name is "pixma:VVVVPPPP_..." AND whose
// USB descriptor actually exposes an interrupt-IN endpoint on interface 0 takes
// this path; every other device falls back to the welland3 SANE poll.

// Parse "pixma:04A91912_498A13" -> vid=0x04a9 pid=0x1912.
static bool pixma_parse_usb_ids(const char* sane_name, uint16_t* vid, uint16_t* pid) {
    if (sane_name == NULL) {
        return false;
    }
    const char* p = strstr(sane_name, "pixma:");
    if (p == NULL) {
        return false;
    }
    p += strlen("pixma:");
    unsigned int v = 0, d = 0;
    if (sscanf(p, "%4x%4x", &v, &d) != 2) {
        return false;
    }
    *vid = (uint16_t)v;
    *pid = (uint16_t)d;
    return true;
}

// If interface 0 of dev exposes an interrupt-IN endpoint, return its address,
// else 0.
static uint8_t pixma_find_int_in_ep(libusb_device* dev) {
    struct libusb_config_descriptor* cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        return 0;
    }
    uint8_t ep = 0;
    if (cfg->bNumInterfaces > 0) {
        const struct libusb_interface* itf = &cfg->interface[0];
        for (int a = 0; a < itf->num_altsetting && ep == 0; a += 1) {
            const struct libusb_interface_descriptor* id = &itf->altsetting[a];
            for (int e = 0; e < id->bNumEndpoints; e += 1) {
                const struct libusb_endpoint_descriptor* ed = &id->endpoint[e];
                if ((ed->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN &&
                    (ed->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                    ep = ed->bEndpointAddress;
                    break;
                }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return ep;
}

// One-shot probe: does a vid:pid device with an interface-0 interrupt-IN
// endpoint exist right now? Used to decide whether to take the interrupt path.
static bool pixma_has_int_endpoint(uint16_t vid, uint16_t pid) {
    libusb_context* ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        return false;
    }
    libusb_device** list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < n && !found; i += 1) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) {
            continue;
        }
        if (dd.idVendor == vid && dd.idProduct == pid && pixma_find_int_in_ep(list[i]) != 0) {
            found = true;
        }
    }
    if (n >= 0) {
        libusb_free_device_list(list, 1);
    }
    libusb_exit(ctx);
    return found;
}

// Open the pixma device by vid/pid, auto-detach the kernel driver, claim
// interface 0. Returns the handle and its interrupt-IN endpoint via *ep, or
// NULL if not present/claimable.
static libusb_device_handle* pixma_open_claim(libusb_context* ctx,
                                              uint16_t vid, uint16_t pid,
                                              uint8_t* ep) {
    libusb_device** list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        return NULL;
    }
    libusb_device_handle* h = NULL;
    for (ssize_t i = 0; i < n; i += 1) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) {
            continue;
        }
        if (dd.idVendor != vid || dd.idProduct != pid) {
            continue;
        }
        uint8_t found = pixma_find_int_in_ep(list[i]);
        if (found == 0) {
            continue;
        }
        if (libusb_open(list[i], &h) != 0) {
            h = NULL;
            continue;
        }
        libusb_set_auto_detach_kernel_driver(h, 1);
        if (libusb_claim_interface(h, 0) != 0) {
            libusb_close(h);
            h = NULL;
            continue;
        }
        *ep = found;
        break;
    }
    libusb_free_device_list(list, 1);
    return h;
}

// pthread cancellation cleanup for the interrupt watcher: on cancel (scanbd
// shutdown, SIGHUP reload, or SIGUSR1 yield-to-saned) release interface 0 and
// tear down libusb so the scanner is freed and nothing is leaked/stuck-claimed.
// Members live in the caller's stack struct (not registers), so they are safe
// across the cleanup longjmp.
typedef struct {
    libusb_context* ctx;
    libusb_device_handle* h;
} pixma_watch_res_t;
static void pixma_watch_cleanup(void* arg) {
    pixma_watch_res_t* r = (pixma_watch_res_t*)arg;
    if (r->h != NULL) {
        libusb_release_interface(r->h, 0);
        libusb_close(r->h);
        r->h = NULL;
    }
    if (r->ctx != NULL) {
        libusb_exit(r->ctx);
        r->ctx = NULL;
    }
}

// Run the configured action script for a decoded button with the SAME env
// contract scanbd's SANE path presents: <target-env>=<code> plus SCANBD_DEVICE
// / SCANBD_ACTION and PATH/PWD/USER/HOME. Interface 0 MUST already be released
// before calling this (the script opens/scans the device). Mirrors scanbd's own
// fork/seteuid/execle action exec.
static void pixma_run_action(const char* device, const char* script,
                             const char* action_name, const char* dev_env,
                             const char* act_env, const char* target_env,
                             long code, int settle_ms) {
    if (script == NULL || strlen(script) == 0 ||
        strcmp(script, SCANBD_NULL_STRING) == 0) {
        slog(SLOG_INFO, "pixma button: no action script configured for target; "
             "decoded code=%ld only", code);
        return;
    }
    char* script_abs = make_script_path_abs(script);
    assert(script_abs != NULL);

    const int N = 8;  // target, device, action, PATH, PWD, USER, HOME, sentinel
    char** env = calloc(N, sizeof(char*));
    assert(env != NULL);
    for (int i = 0; i < N; i += 1) {
        env[i] = calloc(NAME_MAX + 1, sizeof(char));
        assert(env[i] != NULL);
    }
    int e = 0;
    snprintf(env[e++], NAME_MAX, "%s=%ld", target_env ? target_env : "SCANBD_TARGET", code);
    snprintf(env[e++], NAME_MAX, "%s=%s", dev_env ? dev_env : "SCANBD_DEVICE", device);
    snprintf(env[e++], NAME_MAX, "%s=%s", act_env ? act_env : "SCANBD_ACTION",
             action_name ? action_name : "button");
    {
        const char* v = getenv("PATH");
        snprintf(env[e++], NAME_MAX, "PATH=%s", v ? v : "/usr/sbin:/usr/bin:/sbin:/bin");
    }
    {
        char buf[PATH_MAX];
        const char* v = getcwd(buf, sizeof(buf) - 1);
        snprintf(env[e++], NAME_MAX, "PWD=%s", v ? v : "/");
    }
    {
        struct passwd* pw = getpwuid(geteuid());
        snprintf(env[e++], NAME_MAX, "USER=%s", pw ? pw->pw_name : "root");
    }
    {
        struct passwd* pw = getpwuid(geteuid());
        snprintf(env[e++], NAME_MAX, "HOME=%s", pw ? pw->pw_dir : "/root");
    }
    env[e] = NULL;

    if (settle_ms > 0) {
        usleep(settle_ms * 1000);
    }
    slog(SLOG_ERROR, "pixma button EVENT on %s: running action '%s' (%s=%ld): %s",
         device, action_name ? action_name : "button",
         target_env ? target_env : "SCANBD_TARGET", code, script_abs);

    pid_t cpid = fork();
    if (cpid < 0) {
        slog(SLOG_ERROR, "Can't fork for action: %s", strerror(errno));
    } else if (cpid > 0) {
        int status;
        if (waitpid(cpid, &status, 0) < 0) {
            slog(SLOG_ERROR, "waitpid: %s", strerror(errno));
        } else if (WIFEXITED(status)) {
            slog(SLOG_INFO, "action %s exited with status %d", script_abs, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            slog(SLOG_INFO, "action %s signalled %d", script_abs, WTERMSIG(status));
        }
    } else {  // child: mirror scanbd's privilege handling, then exec
        uid_t euid = geteuid();
        gid_t egid = getegid();
        if (seteuid(0) < 0 || setegid(0) < 0 || setgid(egid) < 0 || setuid(euid) < 0) {
            slog(SLOG_ERROR, "drop-priv for action failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        execle(script_abs, script_abs, NULL, env);
        slog(SLOG_ERROR, "execle %s: %s", script_abs, strerror(errno));
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < N - 1; i += 1) {
        free(env[i]);
    }
    free(env);
    free(script_abs);
}

// The event-driven watch loop for a pixma device. Blocks (with a modest
// timeout) on the interrupt endpoint; on each decoded press releases interface
// 0, runs the action, and reclaims. Runs until the polling thread is cancelled.
// Returns early only if libusb cannot be initialised (caller falls back to the
// SANE poll).
//
// LOCKING / CANCELLATION discipline -- mirrors scanbd's own poll loop so it
// coexists with stop_sane_threads() (which locks st->mutex, waits while
// st->triggered, then pthread_cancel()s this thread on shutdown / SIGHUP reload
// / SIGUSR1 yield-to-saned):
//   * called with st->mutex HELD and sane_poll's mutex cleanup handler
//     installed; st->mutex is a NON-recursive mutex;
//   * cancellation stays DISABLED except at explicit testcancel points, and at
//     every such point st->mutex is HELD, so the mutex cleanup handler unlocks
//     it exactly once on cancel;
//   * st->mutex is RELEASED (cancel disabled) across the blocking interrupt
//     read and across the action, so stop_sane_threads can acquire it; a modest
//     read timeout bounds how long it waits (still event-driven: it blocks on
//     the endpoint and fires instantly on a press, ~zero CPU);
//   * st->triggered is set around the action so a concurrent
//     stop_sane_threads / saned scan waits for the action to finish, exactly
//     like the SANE action path;
//   * on cancel the libusb cleanup handler releases interface 0, freeing the
//     scanner for saned.
#define PIXMA_INT_READ_TIMEOUT_MS 700   // bounds yield latency, not a poll rate
static void pixma_interrupt_watch(sane_thread_t* st, uint16_t vid, uint16_t pid,
                                  const char* script, const char* action_name,
                                  const char* dev_env, const char* act_env,
                                  const char* target_env, int settle_ms) {
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    pixma_watch_res_t res = { NULL, NULL };
    if (libusb_init(&res.ctx) != 0) {
        slog(SLOG_ERROR, "libusb_init failed for %s; falling back to SANE poll",
             st->dev->name);
        return;   // st->mutex still held, as on entry
    }

    // Single cleanup handler for the whole loop: releases the current handle
    // (res.h) and tears down libusb (res.ctx) on cancel. Lexically balanced
    // with the pop below (which is unreachable -- the loop only exits via
    // cancellation -- but the macros must be paired in the same block).
    pthread_cleanup_push(pixma_watch_cleanup, &res);
    int backoff = 1;
    while (true) {
        // --- cancellation point: st->mutex HELD ---
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        pthread_testcancel();
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        uint8_t ep = 0;
        res.h = pixma_open_claim(res.ctx, vid, pid, &ep);
        if (res.h == NULL) {
            slog(SLOG_WARN, "pixma %04x:%04x interrupt endpoint not claimable; "
                 "retry in %ds", vid, pid, backoff);
            pthread_mutex_unlock(&st->mutex);   // release while waiting
            sleep(backoff);
            pthread_mutex_lock(&st->mutex);
            backoff = (backoff < 15) ? backoff * 2 : 15;
            continue;
        }
        backoff = 1;
        slog(SLOG_INFO, "welland4: claimed interface 0 on %s, blocking on EP 0x%02x "
             "(event-driven, %dms read window)", st->dev->name, ep,
             PIXMA_INT_READ_TIMEOUT_MS);

        bool reconnect = false;
        while (!reconnect) {
            // --- cancellation point: st->mutex HELD, res.h valid ---
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
            pthread_testcancel();
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

            unsigned char buf[64];
            int transferred = 0;
            // block on the endpoint with the mutex RELEASED so stop_sane_threads
            // can grab it; the timeout only bounds yield latency.
            pthread_mutex_unlock(&st->mutex);
            int rc = libusb_interrupt_transfer(res.h, ep, buf, sizeof(buf),
                                               &transferred, PIXMA_INT_READ_TIMEOUT_MS);
            pthread_mutex_lock(&st->mutex);

            if (rc == LIBUSB_ERROR_TIMEOUT) {
                continue;  // no event this window; loop (blocks on the endpoint)
            }
            if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO ||
                rc == LIBUSB_ERROR_PIPE) {
                slog(SLOG_WARN, "interrupt read on %s: %s; reconnecting",
                     st->dev->name, libusb_error_name(rc));
                reconnect = true;
                break;
            }
            if (rc != 0) {
                slog(SLOG_WARN, "interrupt read: %s", libusb_error_name(rc));
                continue;
            }
            if (transferred <= 19 || buf[4] != 0x01) {
                slog(SLOG_DEBUG, "non-press/short interrupt (len=%d byte4=0x%02x)",
                     transferred, transferred > 4 ? buf[4] : 0);
                continue;
            }
            long code = buf[19];
            const char* name = pixma_lide_button_name(code);
            slog(SLOG_ERROR, "pixma button EVENT on %s: decoded=%s code=%ld "
                 "(byte19, via EP 0x%02x)", st->dev->name, name, code, ep);
            if (code < 1 || code > 6 || strcmp(name, "unknown") == 0) {
                slog(SLOG_WARN, "unmapped pixma button code %ld; ignoring", code);
                continue;
            }
            // Mark active so a concurrent stop_sane_threads / saned scan waits
            // for us, release interface 0 so the action's own scan can open the
            // scanner, run the action, then reclaim.
            st->triggered = true;
            libusb_release_interface(res.h, 0);
            libusb_close(res.h);
            res.h = NULL;                      // handle freed
            pthread_mutex_unlock(&st->mutex);  // release across the action
            pixma_run_action(st->dev->name, script, action_name,
                             dev_env, act_env, target_env, code, settle_ms);
            pthread_mutex_lock(&st->mutex);
            st->triggered = false;
            if (pthread_cond_broadcast(&st->cv) < 0) {   // wake stop_sane_threads
                slog(SLOG_ERROR, "pthread_cond_broadcast failed");
            }
            reconnect = true;                  // reopen + reclaim interface 0
        }
        if (res.h != NULL) {
            libusb_release_interface(res.h, 0);
            libusb_close(res.h);
            res.h = NULL;
        }
    }
    pthread_cleanup_pop(0);   // unreachable; paired with the push above
}

// thread start funktion
// TODO: refactor, this is awfull long!

static void* sane_poll(void* arg) {
#ifdef CANCEL_TEST
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) < 0) {
        slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
    }
#endif
    sane_thread_t* st = (sane_thread_t*)arg;
    assert(st != NULL);
    slog(SLOG_DEBUG, "sane_poll");
    // we only expect the main thread to handle signals
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    
//    static int si = 0; // don't know why this was a static variable -> nonsense in the case of multiple sane_poll threads
    int si = 0;

    // this thread uses the device and the san_thread_t datastructure
    // lock it
    pthread_cleanup_push(sane_thread_cleanup_mutex, ((void*)&st->mutex));
    if (pthread_mutex_lock(&st->mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        pthread_exit(NULL);
    }
    
    // open the device this thread should poll
    SANE_Status status = SANE_STATUS_INVAL;
    if ((status = sane_open(st->dev->name, &st->h)) != SANE_STATUS_GOOD) {
        slog(SLOG_ERROR, "Can't open device %s: %s", st->dev->name, sane_strstatus(status));
        slog(SLOG_WARN, "abandon polling of %s", st->dev->name);
        pthread_exit(NULL);
    }
    // figure out the number of options this device has
    // option 0 (zero) is guaranteed to exist with the total number of
    // options of that device (including option 0)
    st->num_of_options = 0;
    if ((status = sane_control_option(st->h, 0, SANE_ACTION_GET_VALUE,
                                      &st->num_of_options, 0)) != SANE_STATUS_GOOD) {
        slog(SLOG_ERROR, "Can't get the number of scanner options");
        pthread_exit(NULL);
    }
    if (st->num_of_options == 0) {
        // no options -> nothing to poll
        slog(SLOG_INFO, "No options for device %s", st->dev->name);
        pthread_exit(NULL);
    }
    slog(SLOG_INFO, "found %d options for device %s", st->num_of_options, st->dev->name);

    // allocate an array of options for the  matching actions
    //
    // only one script is possible per option, later matching
    // actions overwrite previous ones

    // initialize the list of matching options
    if (st->opts != NULL) {
        slog(SLOG_ERROR, "possible memory leak: %s, %d", __FILE__, __LINE__);
    }
    st->opts = NULL;
    st->opts = (sane_dev_option_t*) calloc(st->num_of_options,
                                           sizeof(sane_dev_option_t));
    assert(st->opts != NULL);
    for(int i = 0; i < st->num_of_options; i += 1) {
        sane_option_value_init(&st->opts[i].from_value);
        sane_option_value_init(&st->opts[i].to_value);
        sane_option_value_init(&st->opts[i].value);
    }

    // the number of valid entries in the above list
    st->num_of_options_with_scripts = 0;

    // initialize the list of matching functions
    if (st->functions != NULL) {
        slog(SLOG_ERROR, "possible memory leak: %s, %d", __FILE__, __LINE__);
    }
    st->functions = NULL;
    st->functions = (sane_dev_function_t*) calloc(st->num_of_options,
                                                  sizeof(sane_dev_function_t));
    assert(st->functions != NULL);
    for(int i = 0; i < st->num_of_options; i += 1) {
        st->functions[i].number = 0;
        st->functions[i].env = NULL;
    }
    // the number of valid entries in the above list
    st->num_of_options_with_functions = 0;

    // find out the functions and actions
    // get the global sconfig section
    cfg_t* cfg_sec_global = NULL;
    cfg_sec_global = cfg_getsec(cfg, C_GLOBAL);
    assert(cfg_sec_global);

    // find the global actions
    sane_find_matching_options(st, cfg_sec_global);

    // find the global functions
    sane_find_matching_functions(st, cfg_sec_global);
    
    // find (if any) device specifc sections
    // these override global definitions, if any
    int local_sections = cfg_size(cfg, C_DEVICE);
    slog(SLOG_DEBUG, "found %d local device sections", local_sections);
    
    for(int loc = 0; loc < local_sections; loc += 1) {
        cfg_t* loc_i = cfg_getnsec(cfg, C_DEVICE, loc);
        assert(loc_i != NULL);

        // get the filter-regex from the config-file
        const char* loc_regex = cfg_getstr(loc_i, C_FILTER);
        assert(loc_regex != NULL);

        const char* title = cfg_title(loc_i);
        if (title == NULL) {
            title = "(none)";
        }
        // compile the filter-regex
        slog(SLOG_INFO, "checking device section %s with filter: %s",
             title, loc_regex);
        regex_t creg;
        int ret = regcomp(&creg, loc_regex, REG_EXTENDED | REG_NOSUB);
        if (ret < 0) {
            char err_text[1024];
            regerror(ret, &creg, err_text, 1024);
            slog(SLOG_WARN, "Can't compile regex: %s : %s", loc_regex, err_text);
            continue;
        }
        // compare the regex against the device name
        if (regexec(&creg, st->dev->name, 0, NULL, 0) == 0) {
            // match
            int loc_actions = cfg_size(loc_i, C_ACTION);
            slog(SLOG_INFO, "found %d local action for device %s [%s]",
                 loc_actions, st->dev->name, title);
            // get the local actions for this device
            sane_find_matching_options(st, loc_i);
            // get the local functions for this device
            sane_find_matching_functions(st, loc_i);
        }
        regfree(&creg);
    } // foreach local section
    
    int timeout = cfg_getint(cfg_sec_global, C_TIMEOUT);
    if (timeout <= 0) {
        timeout = C_TIMEOUT_DEF;
    }
    slog(SLOG_DEBUG, "timeout: %d ms", timeout);

    // welland4: if this is a pixma device that exposes a button interrupt
    // endpoint on interface 0, use the event-driven interrupt path instead of
    // the SANE-option poll. Resolve the target action's script/name and the
    // env-var names from the config scanbd already parsed for this device, then
    // hand interface 0 to libusb (release the SANE handle first).
    {
        uint16_t vid = 0, pid = 0;
        int target_optnum = sane_find_option_by_name(st, "target");
        if (target_optnum > 0 &&
            pixma_parse_usb_ids(st->dev->name, &vid, &pid) &&
            pixma_has_int_endpoint(vid, pid)) {

            const char* script = NULL;
            const char* action_name = NULL;
            for (int k = 0; k < st->num_of_options_with_scripts; k += 1) {
                if (st->opts[k].number == target_optnum) {
                    script = st->opts[k].script;
                    action_name = st->opts[k].action_name;
                    break;
                }
            }
            const char* target_env = "SCANBD_TARGET";
            for (int j = 0; j < st->num_of_options_with_functions; j += 1) {
                if (st->functions[j].number == target_optnum &&
                    st->functions[j].env != NULL) {
                    target_env = st->functions[j].env;
                    break;
                }
            }
            cfg_t* global_envs = cfg_getsec(cfg_sec_global, C_ENVIRONMENT);
            const char* dev_env = global_envs ? cfg_getstr(global_envs, C_DEVICE) : NULL;
            const char* act_env = global_envs ? cfg_getstr(global_envs, C_ACTION) : NULL;

            slog(SLOG_INFO, "welland4: %s exposes a button interrupt endpoint; "
                 "using the event-driven interrupt path (no SANE polling)",
                 st->dev->name);

            // release the SANE handle so libusb can claim interface 0
            sane_close(st->h);
            st->h = NULL;

            pixma_interrupt_watch(st, vid, pid, script, action_name,
                                  dev_env, act_env, target_env, timeout);

            // only reached if libusb could not init: fall back to the SANE poll
            slog(SLOG_WARN, "%s: interrupt path unavailable, reopening for SANE poll",
                 st->dev->name);
            if ((status = sane_open(st->dev->name, &st->h)) != SANE_STATUS_GOOD) {
                slog(SLOG_ERROR, "reopen of %s failed: %s",
                     st->dev->name, sane_strstatus(status));
                pthread_exit(NULL);
            }
        }
    }

    slog(SLOG_DEBUG, "Start the polling for device %s", st->dev->name);
    while(true) {
        slog(SLOG_DEBUG, "polling thread for %s, before cancellation point", st->dev->name);
#ifdef CANCEL_TEST
        if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
        }
#endif
        // special cancellation point
        pthread_testcancel();

#ifdef CANCEL_TEST
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) < 0) {
        slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
    }
#endif
    slog(SLOG_DEBUG, "polling thread for %s, after cancellation point", st->dev->name);

    slog(SLOG_DEBUG, "polling device %s", st->dev->name);

        // Refresh backend-cached button/event state before reading the
        // monitored options this pass (pixma button-fix; see
        // sane_refresh_button_state above). No-op for backends without a
        // "button-update" option.
        sane_refresh_button_state(st);

        for(si = 0; si < st->num_of_options_with_scripts; si += 1) {
            const SANE_Option_Descriptor* odesc = NULL;
            odesc = sane_get_option_descriptor(st->h, st->opts[si].number);
            assert(odesc);

            if (st->opts[si].script != NULL) {
                if (strlen(st->opts[si].script) <= 0) {
                    slog(SLOG_WARN, "No valid script for option %s for device %s",
                         odesc->name, st->dev->name);
                    continue;
                }
            }
            else {
                slog(SLOG_WARN, "No script for option %s for device %s",
                     odesc->name, st->dev->name);
                continue;
            }
            assert(st->opts[si].script != NULL);
            assert(strlen(st->opts[si].script) > 0);

            sane_opt_value_t value;
            sane_option_value_init(&value);
            // push the cleanup-handler to free the value storage
            pthread_cleanup_push(sane_thread_cleanup_value, &value);

            // get the actual value
            // but don't query an option twice or more (see config multiple_actions)
            // because this may reset the values and no other value changes can be
            // detected
            int o = 0;
            bool gotAlready = false;
            for(o = 0; o < si; o += 1) {
                if (st->opts[o].number == st->opts[si].number) {
                    gotAlready = true;
                    break;
                }
            }
            if (!gotAlready) {
                // first query of option with this number
                value = get_sane_option_value(st->h, st->opts[si].number);
            }
            else {
                // additional query, so copy the value
                slog(SLOG_INFO, "got the value already -> copy");
                // found: copy the value
                slog(SLOG_DEBUG, "copy the value of option %d", st->opts[o].number);
                value.num_value = st->opts[o].value.num_value;
                if (st->opts[o].value.str_value.str != NULL) {
                    value.str_value.str = strdup(st->opts[o].value.str_value.str);
                    assert(value.str_value.str != NULL);
                }
            }

            // Log the option's REAL value, dereferenced by SANE type.
            // (value is a sane_opt_value_t struct: the integer/bool/button/
            //  fixed value lives in .num_value, a string in .str_value.str.
            //  The old code passed the whole struct to %d, printing garbage
            //  -- a constant pointer-like number -- which hid the actual
            //  button state.) The "value: " prefix is unchanged.
            if (odesc->type == SANE_TYPE_FIXED) {
                slog(SLOG_INFO, "checking option %s number %d (%d) for device %s: value: %f",
                     odesc->name, st->opts[si].number, si,
                     st->dev->name, SANE_UNFIX((SANE_Fixed)value.num_value));
            }
            else if (odesc->type == SANE_TYPE_STRING) {
                slog(SLOG_INFO, "checking option %s number %d (%d) for device %s: value: %s",
                     odesc->name, st->opts[si].number, si,
                     st->dev->name, value.str_value.str ? value.str_value.str : "(null)");
            }
            else {
                // SANE_TYPE_BOOL (0/1), SANE_TYPE_INT, SANE_TYPE_BUTTON
                slog(SLOG_INFO, "checking option %s number %d (%d) for device %s: value: %lu",
                     odesc->name, st->opts[si].number, si,
                     st->dev->name, value.num_value);
            }

            if ((odesc->type == SANE_TYPE_BOOL) || (odesc->type == SANE_TYPE_INT) ||
                    (odesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
                if ((st->opts[si].from_value.num_value == st->opts[si].value.num_value) &&
                        (st->opts[si].to_value.num_value == value.num_value)) {
                    slog(SLOG_DEBUG, "value trigger: numerical");
                    st->triggered = true;
                    st->triggered_option = si;
                    // we need to trigger all waiting threads
                    if (pthread_cond_broadcast(&st->cv) < 0) {
                        slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
                    }
                }
            }
            else if (odesc->type == SANE_TYPE_STRING) {
                if ((regexec(st->opts[si].from_value.str_value.reg,
                             st->opts[si].value.str_value.str, 0, NULL, 0) == 0) &&
                        (regexec(st->opts[si].to_value.str_value.reg,
                                 value.str_value.str, 0, NULL, 0) == 0)) {
                    slog(SLOG_DEBUG, "value trigger: string");
                    st->triggered = true;
                    st->triggered_option = si;
                    // we need to trigger all waiting threads
                    if (pthread_cond_broadcast(&st->cv) < 0) {
                        slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
                    }
                }
            }
            else {
                assert(false);
            }
            // free the previous allocated value
            sane_option_value_free(&st->opts[si].value);

            // pass the responsibility to free the value to the main
            // thread, if this thread gets canceled
            if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL) < 0) {
                slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
            }
            st->opts[si].value = value;
            pthread_cleanup_pop(0);
            if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) < 0) {
                slog(SLOG_ERROR, "pthread_setcancelstate: %s", strerror(errno));
            }

            // was there a value change?
            if (st->triggered && (st->triggered_option >= 0)) {
                assert(st->triggered_option >= 0); // index into the opts-array
                assert(st->triggered_option < st->num_of_options_with_scripts);

                slog(SLOG_ERROR, "trigger action for %s for device %s with script %s",
                     odesc->name, st->dev->name, st->opts[st->triggered_option].script);

                // prepare the environment for the script to be called

                // number of env-vars =
                // number of found function-options
                // plus the values in the environment-section (2):
                // device, action
                // plus those 4:
                // PATH, PWD, USER, HOME
                // plus the sentinel
                cfg_t* global_envs = cfg_getsec(cfg_sec_global, C_ENVIRONMENT);

                int number_of_envs = st->num_of_options_with_functions + 4 + 2 + 1;
                char** env = calloc(number_of_envs, sizeof(char*));
                for(int e = 0; e < number_of_envs; e += 1) {
                    env[e] = calloc(NAME_MAX + 1, sizeof(char));
                }
                int e = 0;
                for(e = 0; e < st->num_of_options_with_functions; e += 1) {
                    const SANE_Option_Descriptor* fdesc = NULL;
                    fdesc = sane_get_option_descriptor(st->h,
                                                       st->functions[e].number);
                    assert(fdesc);

                    // check if the function-option is the same
                    // as a action-option. If so, use the
                    // action-option value instead of re-get the same
                    // option value, because it is (may be) reset
                    // after the query by the backend

                    sane_opt_value_t v;
                    sane_option_value_init(&v);
                    int o = 0;
                    for(o = 0; o < st->num_of_options_with_scripts; o += 1) {
                        if (st->opts[o].number == st->functions[e].number) {
                            break;
                        }
                    }
                    if (o == st->num_of_options_with_scripts) {
                        // not found: query the value
                        v = get_sane_option_value(st->h, st->functions[e].number);
                    }
                    else {
                        slog(SLOG_DEBUG, "don't re-get the value");
                    }
                    if ((fdesc->type == SANE_TYPE_BOOL) || (fdesc->type == SANE_TYPE_INT) ||
                            (fdesc->type == SANE_TYPE_FIXED) || (odesc->type == SANE_TYPE_BUTTON)) {
                        snprintf(env[e], NAME_MAX, "%s=%lu", st->functions[e].env,
                                 v.num_value);
                        slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    }
                    else if (fdesc->type == SANE_TYPE_STRING) {
                        snprintf(env[e], NAME_MAX, "%s=%s", st->functions[e].env,
                                 v.str_value.str);
                        slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    }
                    else {
                        assert(false);
                    }
                    sane_option_value_free(&v);
                }
                const char* ev = "PATH";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, "/usr/sbin:/usr/bin:/sbin:/bin");
                    slog(SLOG_DEBUG, "No PATH, setting env: %s", env[e]);
                    e += 1;
                }
                ev = "PWD";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    char buf[PATH_MAX];
                    char* ptr = getcwd(buf, PATH_MAX - 1);
                    if (!ptr) {
                        slog(SLOG_ERROR, "can't get pwd");
                    }
                    else {
                        assert(ptr);
                        snprintf(env[e], NAME_MAX, "%s=%s", ev, ptr);
                        slog(SLOG_DEBUG, "No PWD, setting env: %s", env[e]);
                        e += 1;
                    }
                }
                ev = "USER";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    struct passwd* pwd = NULL;
                    pwd = getpwuid(geteuid());
                    assert(pwd);
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, pwd->pw_name);
                    slog(SLOG_DEBUG, "No USER, setting env: %s", env[e]);
                    e += 1;
                }
                ev = "HOME";
                if (getenv(ev) != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, getenv(ev));
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                else {
                    struct passwd* pwd = 0;
                    pwd = getpwuid(geteuid());
                    assert(pwd);
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, pwd->pw_dir);
                    slog(SLOG_DEBUG, "No HOME, setting env: %s", env[e]);
                    e += 1;
                }
                ev = cfg_getstr(global_envs, C_DEVICE);
                if (ev != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev, st->dev->name);
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                ev = cfg_getstr(global_envs, C_ACTION);
                if (ev != NULL) {
                    snprintf(env[e], NAME_MAX, "%s=%s", ev,
                             st->opts[st->triggered_option].action_name);
                    slog(SLOG_DEBUG, "setting env: %s", env[e]);
                    e += 1;
                }
                env[e] = NULL;
                assert(e == number_of_envs-1);

                // sendout an dbus-signal with all the values as
                // arguments
                dbus_send_signal(SCANBD_DBUS_SIGNAL_SCAN_BEGIN, st->dev->name);

                //dbus_send_signal_argv_async(SCANBD_DBUS_SIGNAL_TRIGGER, env);
                dbus_send_signal_argv(SCANBD_DBUS_SIGNAL_TRIGGER, env);
                // the action-script will use the device,
                // so we have to release the device
                sane_close(st->h);
                st->h = NULL;

                assert(st->triggered_option >= 0);
                assert(st->opts[st->triggered_option].script);
                assert(strlen(st->opts[st->triggered_option].script) > 0);

                // need to copy the values because we leave the
                // critical section
                // While doing so, convert the script to an absolute path
                // int triggered_option = st->triggered_option;
          
                char *script_abs = 
                     make_script_path_abs(st->opts[st->triggered_option].script);
                
                assert(script_abs);

                // leave the critical section
                if (pthread_mutex_unlock(&st->mutex) < 0) {
                    // if we can't unlock the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
                    pthread_exit(NULL);
                }

                if (strcmp(script_abs, SCANBD_NULL_STRING) != 0) {

                    assert(timeout > 0);
                    usleep(timeout * 1000); //ms

                    pid_t cpid;
                    if ((cpid = fork()) < 0) {
                        slog(SLOG_ERROR, "Can't fork: %s", strerror(errno));
                    }
                    else if (cpid > 0) { // parent
                        slog(SLOG_INFO, "waiting for child: %s", script_abs);
                        int status;
                        if (waitpid(cpid, &status, 0) < 0) {
                            slog(SLOG_ERROR, "waitpid: %s", strerror(errno));
                        }
                        if (WIFEXITED(status)) {
                            slog(SLOG_INFO, "child %s exited with status: %d",
                                 script_abs, WEXITSTATUS(status));
                        }
                        if (WIFSIGNALED(status)) {
                            slog(SLOG_INFO, "child %s signaled with signal: %d",
                                 script_abs, WTERMSIG(status));
                        }
                    }
                    else { // child
                        uid_t euid = geteuid();
                        uid_t egid = getegid();
                        if (seteuid(0) < 0) {
                            slog(SLOG_DEBUG, "Can't seteuid root: %s", strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        if (setegid(0) < 0) {
                            slog(SLOG_DEBUG, "Can't setegid root: %s", strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        slog(SLOG_DEBUG, "setgid to gid=%d", egid);
                        if (setgid(egid) < 0) {
                            slog(SLOG_DEBUG, "Can't setgid for gid=%d: %s", egid, strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        slog(SLOG_DEBUG, "setuid to uid=%d", euid);
                        if (setuid(euid) < 0) {
                            slog(SLOG_DEBUG, "Can't setuid for uid=%d : %s", euid, strerror(errno));
                            exit(EXIT_FAILURE);
                        } 
                        
                        slog(SLOG_DEBUG, "exec for %s", script_abs);
                        if (access(script_abs, F_OK | X_OK) < 0) {
                            slog(SLOG_ERROR, "access: %s", strerror(errno));
                        }
                        struct stat stat_buf;
                        if (stat(script_abs, &stat_buf) < 0) {
                            slog(SLOG_ERROR, "stat: %s", strerror(errno));
                        }
                        else {
                            slog(SLOG_DEBUG, "octal mode for %s: %lo", script_abs, stat_buf.st_mode);
                            slog(SLOG_DEBUG, "file uid: %ld, file gid: %ld", stat_buf.st_uid, stat_buf.st_gid);
                        }
                        if (execle(script_abs, script_abs, NULL, env) < 0) {
                            slog(SLOG_ERROR, "execlp: %s", strerror(errno));
                        }
                        exit(EXIT_FAILURE); // not reached
                    }
                } // script_abs == SCANBD_NULL_STRING

                assert(script_abs != NULL);
                free(script_abs);

                // free (last element is the sentinel!)
                assert(env != NULL);
                for(int e = 0; e < number_of_envs - 1; e += 1) {
                    assert(env[e] != NULL);
                    free(env[e]);
                }
                free(env);

                // enter the critical section
                if (pthread_mutex_lock(&st->mutex) < 0) {
                    // if we can't get the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
                    pthread_exit(NULL);
                }

                st->triggered = false;
                st->triggered_option = -1; // invalid
                // we need to trigger all waiting threads
                if (pthread_cond_broadcast(&st->cv) < 0) {
                    slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
                }

                // leave the critical section
                if (pthread_mutex_unlock(&st->mutex) < 0) {
                    // if we can't release the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
                    pthread_exit(NULL);
                }
                // sleep the timeout to settle devices, necessary?
                usleep(timeout * 1000); //ms

                // send out the debus signal
                dbus_send_signal(SCANBD_DBUS_SIGNAL_SCAN_END, st->dev->name);

                // enter the critical section
                if (pthread_mutex_lock(&st->mutex) < 0) {
                    // if we can't get the mutex, something is heavily wrong!
                    slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
                    pthread_exit(NULL);
                }

                slog(SLOG_DEBUG, "reopen device %s", st->dev->name);
                if ((status = sane_open(st->dev->name, &st->h)) != SANE_STATUS_GOOD) {
                    slog(SLOG_ERROR, "Can't open device %s, %s",
                         st->dev->name, sane_strstatus(status));
                    if (status == SANE_STATUS_ACCESS_DENIED) {
                        slog(SLOG_WARN, "abandon polling of %s", st->dev->name);
                        pthread_exit(NULL);
                    }
                }
            } // if triggered
        } // foreach option

        // release the mutex

        // because pthread_cleanup_pop is a macro we can't use it here
        // pthread_cleanup_pop(1);
        if (pthread_mutex_unlock(&st->mutex) < 0) {
            // if we can't unlock the mutex, something is heavily wrong!
            slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
            pthread_exit(NULL);
        }

        // sleep the polling timeout
        usleep(timeout * 1000); //ms

        // regain the mutex
        // because pthread_cleanup_push is a macro we can't use it here
//         pthread_cleanup_push(sane_thread_cleanup_mutex, ((void*)&st->mutex));
        if (pthread_mutex_lock(&st->mutex) < 0) {
            // if we can't get the mutex, something is heavily wrong!
            slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
            pthread_exit(NULL);
        }
    }
    pthread_cleanup_pop(1); // release the mutex
    pthread_exit(NULL);
}

// helper to trigger a specified action from another thread
// (e.g. dbus) via an action number
void sane_trigger_action(int number_of_dev, int action) {
    assert(number_of_dev >= 0);
    assert(action >= 0);
    slog(SLOG_DEBUG, "sane_trigger_action device=%d, action=%d", number_of_dev, action);

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    if (num_devices <= 0) {
        slog(SLOG_WARN, "No devices at all");
        goto cleanup_sane;
    }
    if (number_of_dev >= num_devices) {
        slog(SLOG_WARN, "No such device number %d", number_of_dev);
        goto cleanup_sane;
    }

    while(sane_poll_threads == NULL) {
        // no devices actually polling
        slog(SLOG_WARN, "No polling at the moment, waiting ...");
        if (pthread_cond_wait(&sane_cv, &sane_mutex) < 0) {
            slog(SLOG_ERROR, "pthread_cond_wait: ", strerror(errno));
            goto cleanup_sane;
        }
    }
    assert(sane_poll_threads != NULL);
    sane_thread_t* st = &sane_poll_threads[number_of_dev];
    assert(st != NULL);
    
    // this thread uses the device and the sane_thread_t datastructure
    // lock it
    if (pthread_mutex_lock(&st->mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        goto cleanup_sane;
    }

    if (action >= st->num_of_options_with_scripts) {
        slog(SLOG_WARN, "No such action %d for device number %d", action, number_of_dev);
        goto cleanup_dev;
    }

    while(st->triggered == true) {
        slog(SLOG_DEBUG, "sane_trigger_action: an action is active, waiting ...");
        if (pthread_cond_wait(&st->cv, &st->mutex) < 0) {
            slog(SLOG_ERROR, "pthread_cond_wait: %s", strerror(errno));
            goto cleanup_dev;
        }
    }
    
    slog(SLOG_DEBUG, "sane_trigger_action: an action is active, waiting ...");

    st->triggered = true;
    st->triggered_option = action;
    // we need to trigger all waiting threads
    if (pthread_cond_broadcast(&st->cv) < 0) {
        slog(SLOG_ERROR, "pthread_cond_broadcats: this shouln't happen");
    }

cleanup_dev:
    if (pthread_mutex_unlock(&st->mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
    }
cleanup_sane:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
    }
    return;
}

void start_sane_threads(void) {
    slog(SLOG_DEBUG, "start_sane_threads");

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    
    if (sane_poll_threads != NULL) {
        // if there are active threads kill them
        stop_sane_threads();
    }
    // allocate the thread list
    assert(sane_poll_threads == NULL);
    
    if (num_devices == 0) {
        slog(SLOG_ERROR, "no devices, not starting any polling thread");
        goto cleanup;
    } 
    sane_poll_threads = (sane_thread_t*) calloc(num_devices, sizeof(sane_thread_t));
    if (sane_poll_threads == NULL) {
        slog(SLOG_ERROR, "Can't allocate memory for polling threads");
        goto cleanup;
    }
    // starting for each device a seperate thread
    for(int i = 0; i < num_devices; i += 1) {
        slog(SLOG_DEBUG, "Starting poll thread for %s", sane_device_list[i]->name);
        sane_poll_threads[i].tid = 0;
        sane_poll_threads[i].dev = sane_device_list[i];
        sane_poll_threads[i].h = 0;
        sane_poll_threads[i].opts = NULL;
        sane_poll_threads[i].functions = NULL;
        sane_poll_threads[i].num_of_options = 0;
        sane_poll_threads[i].triggered = false;
        sane_poll_threads[i].triggered_option = -1;
        sane_poll_threads[i].num_of_options_with_scripts = 0;
        sane_poll_threads[i].num_of_options_with_functions = 0;

        if (pthread_mutex_init(&sane_poll_threads[i].mutex, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_init: should not happen");
        }
        if (pthread_cond_init(&sane_poll_threads[i].cv, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_cond_init: should not happen");
        }
        if (pthread_create(&sane_poll_threads[i].tid, NULL, sane_poll,
                           (void*)&sane_poll_threads[i]) < 0) {
            slog(SLOG_ERROR, "Can't start sane_poll_thread: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        slog(SLOG_DEBUG, "Thread started for device %s", sane_device_list[i]->name);
    }
    if (pthread_cond_broadcast(&sane_cv)) {
        slog(SLOG_ERROR, "pthread_cond_broadcast: %s", strerror(errno));
    }
cleanup:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
        return;
    }
}

// stops all sane polling threads

void stop_sane_threads(void) {
    slog(SLOG_DEBUG, "stop_sane_threads");

    if (pthread_mutex_lock(&sane_mutex) < 0) {
        // if we can't get the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        return;
    }
    
    if (sane_poll_threads == NULL) {
        // we don't have any active threads
        slog(SLOG_DEBUG, "stop_sane_threads: nothing to stop");
        goto cleanup;
    }
    // sending cancel request to all threads
    for(int i = 0; i < num_devices; i += 1) {
        if (pthread_mutex_lock(&sane_poll_threads[i].mutex) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        }
        while(sane_poll_threads[i].triggered == true) {
            slog(SLOG_DEBUG, "stop_sane_threads: an action is active, waiting ...");

            if (pthread_cond_wait(&sane_poll_threads[i].cv,
                                  &sane_poll_threads[i].mutex) < 0) {
                slog(SLOG_ERROR, "pthread_cond_wait: %s", strerror(errno));
            }
        }
        if (pthread_mutex_unlock(&sane_poll_threads[i].mutex) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_lock: %s", strerror(errno));
        }

        slog(SLOG_DEBUG, "stopping poll thread for device %s", (*(sane_device_list + i))->name);
        if (pthread_cancel(sane_poll_threads[i].tid) < 0) {
            if (errno == ESRCH) {
                slog(SLOG_ERROR, "poll thread for device %s was already cancelled");
            }
            else {
                slog(SLOG_ERROR, "unknown error from pthread_cancel: %s", strerror(errno));
            }
        }
    }
    // waiting for all threads to vanish
    for(int i = 0; i < num_devices; i += 1) {
        slog(SLOG_DEBUG, "waiting for poll thread for device %s",
             (*(sane_device_list + i))->name);
        // joining all threads to prevent memory leaks
        if (pthread_join(sane_poll_threads[i].tid, NULL) < 0) {
            slog(SLOG_ERROR, "pthread_join: %s", strerror(errno));
        }
        sane_poll_threads[i].tid = 0;
        // close the associated device of the thread
        slog(SLOG_DEBUG, "closing device %s", sane_poll_threads[i].dev->name);
        if (sane_poll_threads[i].h != NULL) {
            sane_close(sane_poll_threads[i].h);
            sane_poll_threads[i].h = NULL;
        }
        if (sane_poll_threads[i].opts) {
            slog(SLOG_DEBUG, "freeing opt resources for device %s thread",
                 sane_poll_threads[i].dev->name);
            // free the matching options list of that device / threads
            for (int k = 0; k < sane_poll_threads[i].num_of_options; k += 1) {
                sane_option_value_free(&sane_poll_threads[i].opts[k].from_value);
                sane_option_value_free(&sane_poll_threads[i].opts[k].to_value);
                sane_option_value_free(&sane_poll_threads[i].opts[k].value);
            }
            free(sane_poll_threads[i].opts);
            sane_poll_threads[i].opts = NULL;
        }
        if (sane_poll_threads[i].functions) {
            slog(SLOG_DEBUG, "freeing function resources for device %s thread",
                 sane_poll_threads[i].dev->name);
            free(sane_poll_threads[i].functions);
            sane_poll_threads[i].functions = NULL;
        }

        if (pthread_cond_destroy(&sane_poll_threads[i].cv) < 0) {
            slog(SLOG_ERROR, "pthread_cond_destroy: %s", strerror(errno));
        }
        if (pthread_mutex_destroy(&sane_poll_threads[i].mutex) < 0) {
            slog(SLOG_ERROR, "pthread_mutex_destroy: %s", strerror(errno));
        }
    }
    // free the thread list
    free(sane_poll_threads);
    sane_poll_threads = NULL;
    // no threads active anymore
    if (pthread_cond_broadcast(&sane_cv)) {
        slog(SLOG_ERROR, "pthread_cond_broadcast: %s", strerror(errno));
    }
cleanup:
    if (pthread_mutex_unlock(&sane_mutex) < 0) {
        // if we can't unlock the mutex, something is heavily wrong!
        slog(SLOG_ERROR, "pthread_mutex_unlock: %s", strerror(errno));
        return;
    }
}
