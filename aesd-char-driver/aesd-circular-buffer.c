/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    /**
    * TODO: implement per description
    */
    size_t i;                 
    size_t idx;
    const size_t N = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    // Determine how many valid entries are present
    size_t count;
    if (buffer->full) {
        count = N;
    } else {
        // Empty if indices equal and not full
        if (buffer->in_offs == buffer->out_offs) return NULL;
        int diff;
        int diff = (int)buffer->in_offs - (int)buffer->out_offs;
        if (diff < 0) diff += (int)N;   // avoid modulo
        count = (size_t)diff;
    }

    // Walk from oldest (out_offs) forward, accumulating sizes
    idx = buffer->out_offs;
    for (i = 0; i < count; ++i) {
        const struct aesd_buffer_entry *e = &buffer->entry[idx];

        if (e->size > 0) {
            if (char_offset < e->size) {
                if (entry_offset_byte_rtn) *entry_offset_byte_rtn = char_offset;
                return (struct aesd_buffer_entry *)e; // drop const to match API
            }
            char_offset -= e->size;
        }

        // advance with branch (no %)
        ++idx;
        if (idx >= N) idx = 0;
    }

    // Not enough data to reach char_offset
    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    /**
    * TODO: implement per description
    */

    // Add add_entry to buffer at buffer->in_offs
    buffer->entry[buffer->in_offs] = *add_entry;
    // Advance write index choosing branch method no dividing or modulo, embedded friendly
    buffer->in_offs++;
    // Check if we are at end or rolling over if so, move back to Start
    if (buffer->in_offs >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        buffer->in_offs = 0;
    }
    // If buffer was full, set out_offs to in_offs so data can read newest set
    if (buffer->full) {
       buffer->out_offs = buffer->in_offs;
    }
    // Check if we are now full
    buffer->full = (buffer->in_offs == buffer->out_offs);
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
