
/*
 * #define BIT_FIELD_OFFSET offset
 * #define BIT_FIELD_BYTES  bytes
 * #define BIT_FIELD_NAME   name
 */
#define __bit_field(bit_contents) __bit_field_(REQUEST_NAME, REQUEST_VALUE, REQUEST_IDX_KIND, BIT_FIELD_OFFSET, BIT_FIELD_BYTES, BIT_FIELD_NAME, bit_contents)
#define __bit(bit_offset, bit_name) __bit_(REQUEST_NAME, REQUEST_VALUE, REQUEST_IDX_KIND, BIT_FIELD_OFFSET, BIT_FIELD_BYTES, BIT_FIELD_NAME, bit_offset, bit_name)
