/*
 * ++C - C++ introduction
 * Copyright (C) 2013, 2014, 2015, 2016, 2017 Wilhelm Meier <wilhelm.meier@hs-kl.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sane/sane.h>

int main () {
    SANE_Int sane_version = 0;
    if (sane_init(&sane_version, 0) != SANE_STATUS_GOOD) {
        printf("Can't init sane\n");
        exit(EXIT_FAILURE);
    }
    printf("sane version %d.%d\n", SANE_VERSION_MAJOR(sane_version), SANE_VERSION_MINOR(sane_version));
    
    int count = 0;
    while (++count < 10) {
        printf("Iterate\n");
        const SANE_Device** device_list = NULL;
        if (sane_get_devices(&device_list, SANE_TRUE) == SANE_STATUS_GOOD) {
            const SANE_Device** dev = device_list;
            while (*dev) {
                printf("dev name: %s\n", (*dev)->name);
                dev++;
            }
        } else {
            printf("Can't get dev list\n");
            exit(EXIT_FAILURE);
        }
        sleep(1);
    }
    printf("before sane_exit\n");
    sane_exit();
    printf("after sane_exit\n");
    exit(EXIT_SUCCESS);
}