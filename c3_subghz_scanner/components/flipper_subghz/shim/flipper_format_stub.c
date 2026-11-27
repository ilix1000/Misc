/* Saving/loading .sub files is not needed for live decoding: stubs. */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <lib/flipper_format/flipper_format.h>
bool flipper_format_read_uint32( FlipperFormat* flipper_format, const char* key, uint32_t* data, const uint16_t data_size) { return false; }
bool flipper_format_rewind(FlipperFormat* flipper_format) { return false; }
bool flipper_format_update_hex( FlipperFormat* flipper_format, const char* key, const uint8_t* data, const uint16_t data_size) { return false; }
bool flipper_format_write_uint32( FlipperFormat* flipper_format, const char* key, const uint32_t* data, const uint16_t data_size) { return false; }
bool flipper_format_write_string_cstr( FlipperFormat* flipper_format, const char* key, const char* data) { return false; }
bool flipper_format_read_hex( FlipperFormat* flipper_format, const char* key, uint8_t* data, const uint16_t data_size) { return false; }
bool flipper_format_write_hex( FlipperFormat* flipper_format, const char* key, const uint8_t* data, const uint16_t data_size) { return false; }
bool flipper_format_insert_or_update_hex( FlipperFormat* flipper_format, const char* key, const uint8_t* data, const uint16_t data_size) { return false; }
bool flipper_format_write_header_cstr( FlipperFormat* flipper_format, const char* filetype, const uint32_t version) { return false; }
bool flipper_format_read_string(FlipperFormat* flipper_format, const char* key, FuriString* data) { return false; }
bool flipper_format_insert_or_update_uint32( FlipperFormat* flipper_format, const char* key, const uint32_t* data, const uint16_t data_size) { return false; }
bool flipper_format_write_float( FlipperFormat* flipper_format, const char* key, const float* data, const uint16_t data_size) { return false; }
bool flipper_format_update_uint32( FlipperFormat* flipper_format, const char* key, const uint32_t* data, const uint16_t data_size) { return false; }
bool flipper_format_update_string_cstr( FlipperFormat* flipper_format, const char* key, const char* data) { return false; }
bool flipper_format_read_float( FlipperFormat* flipper_format, const char* key, float* data, const uint16_t data_size) { return false; }
bool flipper_format_read_bool( FlipperFormat* flipper_format, const char* key, bool* data, const uint16_t data_size) { return false; }
void flipper_format_free(FlipperFormat* flipper_format) {  }
bool flipper_format_file_close(FlipperFormat* flipper_format) { return false; }
bool flipper_format_write_int32( FlipperFormat* flipper_format, const char* key, const int32_t* data, const uint16_t data_size) { return false; }
bool flipper_format_file_open_always(FlipperFormat* flipper_format, const char* path) { return false; }
FlipperFormat* flipper_format_file_alloc(Storage* storage) { return NULL; }
Stream* flipper_format_get_raw_stream(FlipperFormat* flipper_format) { return NULL; }
bool stream_clean(Stream* stream) { (void)stream; return false; }
