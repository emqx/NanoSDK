//- message_time.h

#pragma once

static long long getNumberFromString(char *theString) {
    int base = 10;
    char *endptr;

    errno = 0;    /* To distinguish success/failure after call */

    long long theValue = strtoll(theString, &endptr, base);

    /* Check for various possible errors */

    if (errno != 0) {
        perror("strtol");
        return 0;
    }

    if (endptr == theString) {
        fprintf(stderr, "No digits were found\n");
        return 0;
    }

    return theValue;
}

static long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return (((long long) tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

