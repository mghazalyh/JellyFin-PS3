#ifndef JF_PES_QUEUE_RESERVE_H
#define JF_PES_QUEUE_RESERVE_H

/* Caller owns the queue lock. Reserve space for a complete incoming PES.
 * If eviction is required, discard through the end of the oldest PES,
 * including an incomplete tail if its head has already been consumed.
 * A return of -1 means invalid input or a PES larger than the entire queue.
 */
static inline int pes_queue_discard_count(const unsigned char *continuation,
                                         int capacity, int read, int used,
                                         int required)
{
    if (!continuation || capacity <= 0 || read < 0 || read >= capacity ||
        used < 0 || used > capacity || required <= 0 || required > capacity)
        return -1;
    int discard = 0;
    while (capacity - used + discard < required) {
        do {
            ++discard;
            if (++read == capacity) read = 0;
        } while (discard < used && continuation[read]);
    }
    return discard;
}

#endif
