#pragma once
#include <furi.h>
#include <lib/toolbox/stream/stream.h>
typedef struct FlipperFormat FlipperFormat;
typedef struct Storage Storage;
bool flipper_format_read_uint32( FlipperFormat* flipper_format, const char* key, uint32_t* data, const uint16_t data_size);
bool flipper_format_rewind(FlipperFormat* flipper_format);
bool flipper_format_update_hex( FlipperFormat* flipper_format, const char* key, const uint8_t* data, const uint16_t data_size);
bool flipper_format_write_uint32( FlipperFormat* flipper_format, const char* key, const uint32_t* data, const uint16_t data_size);
bool flipper_format_write_string_cstr( FlipperFormat* flipper_format, const char* key, const char* data);
bool flipper_format_read_hex( FlipperFormat* flipper_format, const char* key, uint8_t* data, const uint16_t data_size);
bool flipper_format_write_hex( FlipperFormat* flipper_format, const char* key, const uint8_t* data, const uint16_t data_size);
bool flipper_format_insert_or_update_hex( FlipperFormat* flipper_format, const char* key, const uint8_t* data, const uint16_t data_size);
bool flipper_format_write_header_cstr( FlipperFormat* flipper_format, const char* filetype, const uint32_t version);
bool flipper_format_read_string(FlipperFormat* flipper_format, const char* key, FuriString* data);
bool flipper_format_insert_or_update_uint32( FlipperFormat* flipper_format, const char* key, const uint32_t* data, const uint16_t data_size);
bool flipper_format_write_float( FlipperFormat* flipper_format, const char* key, const float* data, const uint16_t data_size);
bool flipper_format_update_uint32( FlipperFormat* flipper_format, const char* key, const uint32_t* data, const uint16_t data_size);
bool flipper_format_update_string_cstr( FlipperFormat* flipper_format, const char* key, const char* data);
bool flipper_format_read_float( FlipperFormat* flipper_format, const char* key, float* data, const uint16_t data_size);
bool flipper_format_read_bool( FlipperFormat* flipper_format, const char* key, bool* data, const uint16_t data_size);
void flipper_format_free(FlipperFormat* flipper_format);
bool flipper_format_file_close(FlipperFormat* flipper_format);
bool flipper_format_write_int32( FlipperFormat* flipper_format, const char* key, const int32_t* data, const uint16_t data_size);
bool flipper_format_file_open_always(FlipperFormat* flipper_format, const char* path);
FlipperFormat* flipper_format_file_alloc(Storage* storage);
Stream* flipper_format_get_raw_stream(FlipperFormat* flipper_format);
