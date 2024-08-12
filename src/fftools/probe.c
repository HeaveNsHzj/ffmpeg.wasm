
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <emscripten.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>
#include <libavutil/bprint.h>
#include <libavutil/hash.h>
#include <libavutil/macros.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include "libavutil/pixdesc.h"
#include "libavutil/intreadwrite.h"
#include <libavutil/avassert.h>
#include "libavutil/stereo3d.h"
#include "libavutil/spherical.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/hdr_dynamic_vivid_metadata.h"
#include "libavutil/dovi_meta.h"
#include "libavutil/display.h"


static char *print_format = "json";

static AVDictionary *format_opts, *codec_opts;

static void uninit_opts(void)
{
    // av_dict_free(&swr_opts);
    // av_dict_free(&sws_dict);
    av_dict_free(&format_opts);
    av_dict_free(&codec_opts);
}

static void (*program_exit)(int ret);

static void register_exit(void (*cb)(int ret))
{
    program_exit = cb;
}

static void exit_program(int ret)
{
    if (program_exit)
        program_exit(ret);

    exit(ret);
}


struct unit_value {
    union { double d; long long int i; } val;
    const char *unit;
};

static const char unit_second_str[]         = "s"    ;
static const char unit_hertz_str[]          = "Hz"   ;
static const char unit_byte_str[]           = "byte" ;
static const char unit_bit_per_second_str[] = "bit/s";

static const struct {
    double bin_val;
    double dec_val;
    const char *bin_str;
    const char *dec_str;
} si_prefixes[] = {
    { 1.0, 1.0, "", "" },
    { 1.024e3, 1e3, "Ki", "K" },
    { 1.048576e6, 1e6, "Mi", "M" },
    { 1.073741824e9, 1e9, "Gi", "G" },
    { 1.099511627776e12, 1e12, "Ti", "T" },
    { 1.125899906842624e15, 1e15, "Pi", "P" },
};


static char *value_string(char *buf, int buf_size, struct unit_value uv)
{
    double vald;
    long long int vali;
    int show_float = 0;

    if (uv.unit == unit_second_str) {
        vald = uv.val.d;
        show_float = 1;
    } else {
        vald = vali = uv.val.i;
    }

    if (uv.unit == unit_second_str && 0) {
        double secs;
        int hours, mins;
        secs  = vald;
        mins  = (int)secs / 60;
        secs  = secs - mins * 60;
        hours = mins / 60;
        mins %= 60;
        snprintf(buf, buf_size, "%d:%02d:%09.6f", hours, mins, secs);
    } else {
        const char *prefix_string = "";

        if (0 && vald > 1) {
            long long int index;

            if (uv.unit == unit_byte_str && 0) {
                index = (long long int) (log2(vald)) / 10;
                index = av_clip(index, 0, FF_ARRAY_ELEMS(si_prefixes) - 1);
                vald /= si_prefixes[index].bin_val;
                prefix_string = si_prefixes[index].bin_str;
            } else {
                index = (long long int) (log10(vald)) / 3;
                index = av_clip(index, 0, FF_ARRAY_ELEMS(si_prefixes) - 1);
                vald /= si_prefixes[index].dec_val;
                prefix_string = si_prefixes[index].dec_str;
            }
            vali = vald;
        }

        if (show_float || (0 && vald != (long long int)vald))
            snprintf(buf, buf_size, "%f", vald);
        else
            snprintf(buf, buf_size, "%lld", vali);
        av_strlcatf(buf, buf_size, "%s%s%s", *prefix_string || 0 ? " " : "",
                 prefix_string, 0 ? uv.unit : "");
    }

    return buf;
}




typedef enum {
    WRITER_STRING_VALIDATION_FAIL,
    WRITER_STRING_VALIDATION_REPLACE,
    WRITER_STRING_VALIDATION_IGNORE,
    WRITER_STRING_VALIDATION_NB
} StringValidation;


typedef struct InputStream {
    AVStream *st;

    AVCodecContext *dec_ctx;
} InputStream;

typedef struct InputFile {
    AVFormatContext *fmt_ctx;

    InputStream *streams;
    int       nb_streams;
} InputFile;

static int nb_streams;
static uint64_t *nb_streams_packets;
static uint64_t *nb_streams_frames;
static int *selected_streams;

static struct AVHashContext *hash;

#define SECTION_FLAG_IS_WRAPPER      1 ///< the section only contains other sections, but has no data at its own level
#define SECTION_FLAG_IS_ARRAY        2 ///< the section contains an array of elements of the same type
#define SECTION_FLAG_HAS_VARIABLE_FIELDS 4 ///< the section may contain a variable number of fields with variable keys.
 

#define WRITER_FLAG_PUT_PACKETS_AND_FRAMES_IN_SAME_CHAPTER 2

typedef enum {
    SECTION_ID_NONE = -1,
    SECTION_ID_CHAPTER,
    SECTION_ID_CHAPTER_TAGS,
    SECTION_ID_CHAPTERS,
    SECTION_ID_ERROR,
    SECTION_ID_FORMAT,
    SECTION_ID_FORMAT_TAGS,
    SECTION_ID_FRAME,
    SECTION_ID_FRAMES,
    SECTION_ID_FRAME_TAGS,
    SECTION_ID_FRAME_SIDE_DATA_LIST,
    SECTION_ID_FRAME_SIDE_DATA,
    SECTION_ID_FRAME_SIDE_DATA_TIMECODE_LIST,
    SECTION_ID_FRAME_SIDE_DATA_TIMECODE,
    SECTION_ID_FRAME_SIDE_DATA_COMPONENT_LIST,
    SECTION_ID_FRAME_SIDE_DATA_COMPONENT,
    SECTION_ID_FRAME_SIDE_DATA_PIECE_LIST,
    SECTION_ID_FRAME_SIDE_DATA_PIECE,
    SECTION_ID_FRAME_LOG,
    SECTION_ID_FRAME_LOGS,
    SECTION_ID_LIBRARY_VERSION,
    SECTION_ID_LIBRARY_VERSIONS,
    SECTION_ID_PACKET,
    SECTION_ID_PACKET_TAGS,
    SECTION_ID_PACKETS,
    SECTION_ID_PACKETS_AND_FRAMES,
    SECTION_ID_PACKET_SIDE_DATA_LIST,
    SECTION_ID_PACKET_SIDE_DATA,
    SECTION_ID_PIXEL_FORMAT,
    SECTION_ID_PIXEL_FORMAT_FLAGS,
    SECTION_ID_PIXEL_FORMAT_COMPONENT,
    SECTION_ID_PIXEL_FORMAT_COMPONENTS,
    SECTION_ID_PIXEL_FORMATS,
    SECTION_ID_PROGRAM_STREAM_DISPOSITION,
    SECTION_ID_PROGRAM_STREAM_TAGS,
    SECTION_ID_PROGRAM,
    SECTION_ID_PROGRAM_STREAMS,
    SECTION_ID_PROGRAM_STREAM,
    SECTION_ID_PROGRAM_TAGS,
    SECTION_ID_PROGRAM_VERSION,
    SECTION_ID_PROGRAMS,
    SECTION_ID_ROOT,
    SECTION_ID_STREAM,
    SECTION_ID_STREAM_DISPOSITION,
    SECTION_ID_STREAMS,
    SECTION_ID_STREAM_TAGS,
    SECTION_ID_STREAM_SIDE_DATA_LIST,
    SECTION_ID_STREAM_SIDE_DATA,
    SECTION_ID_SUBTITLE,
} SectionID;

#define SECTION_MAX_NB_LEVELS 10

#define SECTION_MAX_NB_CHILDREN 10

struct section {
    int id;             ///< unique id identifying a section
    const char *name;

#define SECTION_FLAG_IS_WRAPPER      1 ///< the section only contains other sections, but has no data at its own level
#define SECTION_FLAG_IS_ARRAY        2 ///< the section contains an array of elements of the same type
#define SECTION_FLAG_HAS_VARIABLE_FIELDS 4 ///< the section may contain a variable number of fields with variable keys.
                                           ///  For these sections the element_name field is mandatory.
    int flags;
    int children_ids[SECTION_MAX_NB_CHILDREN+1]; ///< list of children section IDS, terminated by -1
    const char *element_name; ///< name of the contained element, if provided
    const char *unique_name;  ///< unique section name, in case the name is ambiguous
    AVDictionary *entries_to_show;
    int show_all_entries;
};


static struct section sections[] = {
    [SECTION_ID_CHAPTERS] =           { SECTION_ID_CHAPTERS, "chapters", SECTION_FLAG_IS_ARRAY, { SECTION_ID_CHAPTER, -1 } },
    [SECTION_ID_CHAPTER] =            { SECTION_ID_CHAPTER, "chapter", 0, { SECTION_ID_CHAPTER_TAGS, -1 } },
    [SECTION_ID_CHAPTER_TAGS] =       { SECTION_ID_CHAPTER_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "chapter_tags" },
    [SECTION_ID_ERROR] =              { SECTION_ID_ERROR, "error", 0, { -1 } },
    [SECTION_ID_FORMAT] =             { SECTION_ID_FORMAT, "format", 0, { SECTION_ID_FORMAT_TAGS, -1 }, .show_all_entries = 1 },
    [SECTION_ID_FORMAT_TAGS] =        { SECTION_ID_FORMAT_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "format_tags", .show_all_entries = 1 },
    [SECTION_ID_FRAMES] =             { SECTION_ID_FRAMES, "frames", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FRAME, SECTION_ID_SUBTITLE, -1 } },
    [SECTION_ID_FRAME] =              { SECTION_ID_FRAME, "frame", 0, { SECTION_ID_FRAME_TAGS, SECTION_ID_FRAME_SIDE_DATA_LIST, SECTION_ID_FRAME_LOGS, -1 } },
    [SECTION_ID_FRAME_TAGS] =         { SECTION_ID_FRAME_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "frame_tags" },
    [SECTION_ID_FRAME_SIDE_DATA_LIST] ={ SECTION_ID_FRAME_SIDE_DATA_LIST, "side_data_list", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FRAME_SIDE_DATA, -1 }, .element_name = "side_data", .unique_name = "frame_side_data_list" },
    [SECTION_ID_FRAME_SIDE_DATA] =     { SECTION_ID_FRAME_SIDE_DATA, "side_data", 0, { SECTION_ID_FRAME_SIDE_DATA_TIMECODE_LIST, SECTION_ID_FRAME_SIDE_DATA_COMPONENT_LIST, -1 }, .unique_name = "frame_side_data" },
    [SECTION_ID_FRAME_SIDE_DATA_TIMECODE_LIST] =  { SECTION_ID_FRAME_SIDE_DATA_TIMECODE_LIST, "timecodes", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FRAME_SIDE_DATA_TIMECODE, -1 } },
    [SECTION_ID_FRAME_SIDE_DATA_TIMECODE] =       { SECTION_ID_FRAME_SIDE_DATA_TIMECODE, "timecode", 0, { -1 } },
    [SECTION_ID_FRAME_SIDE_DATA_COMPONENT_LIST] = { SECTION_ID_FRAME_SIDE_DATA_COMPONENT_LIST, "components", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FRAME_SIDE_DATA_COMPONENT, -1 } },
    [SECTION_ID_FRAME_SIDE_DATA_COMPONENT] =      { SECTION_ID_FRAME_SIDE_DATA_COMPONENT, "component", 0, { SECTION_ID_FRAME_SIDE_DATA_PIECE_LIST, -1 } },
    [SECTION_ID_FRAME_SIDE_DATA_PIECE_LIST] =   { SECTION_ID_FRAME_SIDE_DATA_PIECE_LIST, "pieces", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FRAME_SIDE_DATA_PIECE, -1 } },
    [SECTION_ID_FRAME_SIDE_DATA_PIECE] =        { SECTION_ID_FRAME_SIDE_DATA_PIECE, "section", 0, { -1 } },
    [SECTION_ID_FRAME_LOGS] =         { SECTION_ID_FRAME_LOGS, "logs", SECTION_FLAG_IS_ARRAY, { SECTION_ID_FRAME_LOG, -1 } },
    [SECTION_ID_FRAME_LOG] =          { SECTION_ID_FRAME_LOG, "log", 0, { -1 },  },
    [SECTION_ID_LIBRARY_VERSIONS] =   { SECTION_ID_LIBRARY_VERSIONS, "library_versions", SECTION_FLAG_IS_ARRAY, { SECTION_ID_LIBRARY_VERSION, -1 } },
    [SECTION_ID_LIBRARY_VERSION] =    { SECTION_ID_LIBRARY_VERSION, "library_version", 0, { -1 } },
    [SECTION_ID_PACKETS] =            { SECTION_ID_PACKETS, "packets", SECTION_FLAG_IS_ARRAY, { SECTION_ID_PACKET, -1} },
    [SECTION_ID_PACKETS_AND_FRAMES] = { SECTION_ID_PACKETS_AND_FRAMES, "packets_and_frames", SECTION_FLAG_IS_ARRAY, { SECTION_ID_PACKET, -1} },
    [SECTION_ID_PACKET] =             { SECTION_ID_PACKET, "packet", 0, { SECTION_ID_PACKET_TAGS, SECTION_ID_PACKET_SIDE_DATA_LIST, -1 } },
    [SECTION_ID_PACKET_TAGS] =        { SECTION_ID_PACKET_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "packet_tags" },
    [SECTION_ID_PACKET_SIDE_DATA_LIST] ={ SECTION_ID_PACKET_SIDE_DATA_LIST, "side_data_list", SECTION_FLAG_IS_ARRAY, { SECTION_ID_PACKET_SIDE_DATA, -1 }, .element_name = "side_data", .unique_name = "packet_side_data_list" },
    [SECTION_ID_PACKET_SIDE_DATA] =     { SECTION_ID_PACKET_SIDE_DATA, "side_data", 0, { -1 }, .unique_name = "packet_side_data" },
    [SECTION_ID_PIXEL_FORMATS] =      { SECTION_ID_PIXEL_FORMATS, "pixel_formats", SECTION_FLAG_IS_ARRAY, { SECTION_ID_PIXEL_FORMAT, -1 } },
    [SECTION_ID_PIXEL_FORMAT] =       { SECTION_ID_PIXEL_FORMAT, "pixel_format", 0, { SECTION_ID_PIXEL_FORMAT_FLAGS, SECTION_ID_PIXEL_FORMAT_COMPONENTS, -1 }, .show_all_entries = 1 },
    [SECTION_ID_PIXEL_FORMAT_FLAGS] = { SECTION_ID_PIXEL_FORMAT_FLAGS, "flags", 0, { -1 }, .unique_name = "pixel_format_flags" },
    [SECTION_ID_PIXEL_FORMAT_COMPONENTS] = { SECTION_ID_PIXEL_FORMAT_COMPONENTS, "components", SECTION_FLAG_IS_ARRAY, {SECTION_ID_PIXEL_FORMAT_COMPONENT, -1 }, .unique_name = "pixel_format_components" },
    [SECTION_ID_PIXEL_FORMAT_COMPONENT]  = { SECTION_ID_PIXEL_FORMAT_COMPONENT, "component", 0, { -1 } },
    [SECTION_ID_PROGRAM_STREAM_DISPOSITION] = { SECTION_ID_PROGRAM_STREAM_DISPOSITION, "disposition", 0, { -1 }, .unique_name = "program_stream_disposition" },
    [SECTION_ID_PROGRAM_STREAM_TAGS] =        { SECTION_ID_PROGRAM_STREAM_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "program_stream_tags" },
    [SECTION_ID_PROGRAM] =                    { SECTION_ID_PROGRAM, "program", 0, { SECTION_ID_PROGRAM_TAGS, SECTION_ID_PROGRAM_STREAMS, -1 } },
    [SECTION_ID_PROGRAM_STREAMS] =            { SECTION_ID_PROGRAM_STREAMS, "streams", SECTION_FLAG_IS_ARRAY, { SECTION_ID_PROGRAM_STREAM, -1 }, .unique_name = "program_streams" },
    [SECTION_ID_PROGRAM_STREAM] =             { SECTION_ID_PROGRAM_STREAM, "stream", 0, { SECTION_ID_PROGRAM_STREAM_DISPOSITION, SECTION_ID_PROGRAM_STREAM_TAGS, -1 }, .unique_name = "program_stream", .show_all_entries = 1 },
    [SECTION_ID_PROGRAM_TAGS] =               { SECTION_ID_PROGRAM_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "program_tags" },
    [SECTION_ID_PROGRAM_VERSION] =    { SECTION_ID_PROGRAM_VERSION, "program_version", 0, { -1 } },
    [SECTION_ID_PROGRAMS] =                   { SECTION_ID_PROGRAMS, "programs", SECTION_FLAG_IS_ARRAY, { SECTION_ID_PROGRAM, -1 } },
    [SECTION_ID_ROOT] =               { SECTION_ID_ROOT, "root", SECTION_FLAG_IS_WRAPPER,
                                        { SECTION_ID_CHAPTERS, SECTION_ID_FORMAT, SECTION_ID_FRAMES, SECTION_ID_PROGRAMS, SECTION_ID_STREAMS,
                                          SECTION_ID_PACKETS, SECTION_ID_ERROR, SECTION_ID_PROGRAM_VERSION, SECTION_ID_LIBRARY_VERSIONS,
                                          SECTION_ID_PIXEL_FORMATS, -1} },
    [SECTION_ID_STREAMS] =            { SECTION_ID_STREAMS, "streams", SECTION_FLAG_IS_ARRAY, { SECTION_ID_STREAM, -1 },.show_all_entries = 1 },
    [SECTION_ID_STREAM] =             { SECTION_ID_STREAM, "stream", 0, { SECTION_ID_STREAM_DISPOSITION, SECTION_ID_STREAM_TAGS, SECTION_ID_STREAM_SIDE_DATA_LIST, -1 },.show_all_entries = 1 },
    [SECTION_ID_STREAM_DISPOSITION] = { SECTION_ID_STREAM_DISPOSITION, "disposition", 0, { -1 }, .unique_name = "stream_disposition", .show_all_entries = 1 },
    [SECTION_ID_STREAM_TAGS] =        { SECTION_ID_STREAM_TAGS, "tags", SECTION_FLAG_HAS_VARIABLE_FIELDS, { -1 }, .element_name = "tag", .unique_name = "stream_tags", .show_all_entries = 1 },
    [SECTION_ID_STREAM_SIDE_DATA_LIST] ={ SECTION_ID_STREAM_SIDE_DATA_LIST, "side_data_list", SECTION_FLAG_IS_ARRAY, { SECTION_ID_STREAM_SIDE_DATA, -1 }, .element_name = "side_data", .unique_name = "stream_side_data_list" },
    [SECTION_ID_STREAM_SIDE_DATA] =     { SECTION_ID_STREAM_SIDE_DATA, "side_data", 0, { -1 }, .unique_name = "stream_side_data" },
    [SECTION_ID_SUBTITLE] =           { SECTION_ID_SUBTITLE, "subtitle", 0, { -1 } },
};

static void ffprobe_cleanup(int ret)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(sections); i++)
        av_dict_free(&(sections[i].entries_to_show));
}


/* FFprobe context */
static const char *input_filename;
static const char *print_input_filename;
static const AVInputFormat *iformat = NULL;
static const char *output_filename = NULL;


static void print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

typedef struct WriterContext WriterContext;

typedef struct Writer {
    const AVClass *priv_class;      ///< private class of the writer, if any
    int priv_size;                  ///< private size for the writer context
    const char *name;

    int  (*init)  (WriterContext *wctx);
    void (*uninit)(WriterContext *wctx);

    void (*print_section_header)(WriterContext *wctx);
    void (*print_section_footer)(WriterContext *wctx);
    void (*print_integer)       (WriterContext *wctx, const char *, long long int);
    void (*print_rational)      (WriterContext *wctx, AVRational *q, char *sep);
    void (*print_string)        (WriterContext *wctx, const char *, const char *);
    int flags;                  ///< a combination or WRITER_FLAG_*
} Writer;

/* JSON output */

typedef struct JSONContext {
    const AVClass *class;
    int indent_level;
    int compact;
    const char *item_sep, *item_start_end;
} JSONContext;


/* WRITERS */

#define DEFINE_WRITER_CLASS(name)                   \
static const char *name##_get_name(void *ctx)       \
{                                                   \
    return #name ;                                  \
}                                                   \
static const AVClass name##_class = {               \
    .class_name = #name,                            \
    .item_name  = name##_get_name,                  \
    .option     = name##_options                    \
}

struct WriterContext {
    const AVClass *class;           ///< class of the writer
    const Writer *writer;           ///< the Writer of which this is an instance
    AVIOContext *avio;              ///< the I/O context used to write

    void (* writer_w8)(WriterContext *wctx, int b);
    void (* writer_put_str)(WriterContext *wctx, const char *str);
    void (* writer_printf)(WriterContext *wctx, const char *fmt, ...);

    char *name;                     ///< name of this writer instance
    void *priv;                     ///< private data for use by the filter

    const struct section *sections; ///< array containing all sections
    int nb_sections;                ///< number of sections

    int level;                      ///< current level, starting from 0

    /** number of the item printed in the given section, starting from 0 */
    unsigned int nb_item[SECTION_MAX_NB_LEVELS];

    /** section per each level */
    const struct section *section[SECTION_MAX_NB_LEVELS];
    AVBPrint section_pbuf[SECTION_MAX_NB_LEVELS]; ///< generic print buffer dedicated to each section,
                                                  ///  used by various writers

    unsigned int nb_section_packet; ///< number of the packet section in case we are in "packets_and_frames" section
    unsigned int nb_section_frame;  ///< number of the frame  section in case we are in "packets_and_frames" section
    unsigned int nb_section_packet_frame; ///< nb_section_packet or nb_section_frame according if is_packets_and_frames

    int string_validation;
    char *string_validation_replacement;
    unsigned int string_validation_utf8_flags;
};

#undef OFFSET
#define OFFSET(x) offsetof(JSONContext, x)

static const AVOption json_options[]= {
    { "compact", "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { "c",       "enable compact output", OFFSET(compact), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1 },
    { NULL }
};


static void bprint_bytes(AVBPrint *bp, const uint8_t *ubuf, size_t ubuf_size)
{
    int i;
    av_bprintf(bp, "0X");
    for (i = 0; i < ubuf_size; i++)
        av_bprintf(bp, "%02X", ubuf[i]);
}

static inline void writer_w8_avio(WriterContext *wctx, int b)
{
    avio_w8(wctx->avio, b);
}

static inline void writer_put_str_avio(WriterContext *wctx, const char *str)
{
    avio_write(wctx->avio, str, strlen(str));
}

static inline void writer_printf_avio(WriterContext *wctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    avio_vprintf(wctx->avio, fmt, ap);
    va_end(ap);
}

static inline void writer_w8_printf(WriterContext *wctx, int b)
{
    printf("%c", b);
}

static inline void writer_put_str_printf(WriterContext *wctx, const char *str)
{
    printf("%s", str);
}

static inline void writer_printf_printf(WriterContext *wctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}



static const char *writer_get_name(void *p)
{
    WriterContext *wctx = p;
    return wctx->writer->name;
}
#define OFFSET(x) offsetof(WriterContext, x)

static const AVOption writer_options[] = {
    { "string_validation", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, {.i64=WRITER_STRING_VALIDATION_REPLACE}, 0, WRITER_STRING_VALIDATION_NB-1, .unit = "sv" },
    { "sv", "set string validation mode",
      OFFSET(string_validation), AV_OPT_TYPE_INT, {.i64=WRITER_STRING_VALIDATION_REPLACE}, 0, WRITER_STRING_VALIDATION_NB-1, .unit = "sv" },
    { "ignore",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = WRITER_STRING_VALIDATION_IGNORE},  .unit = "sv" },
    { "replace", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = WRITER_STRING_VALIDATION_REPLACE}, .unit = "sv" },
    { "fail",    NULL, 0, AV_OPT_TYPE_CONST, {.i64 = WRITER_STRING_VALIDATION_FAIL},    .unit = "sv" },
    { "string_validation_replacement", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, {.str=""}},
    { "svr", "set string validation replacement string", OFFSET(string_validation_replacement), AV_OPT_TYPE_STRING, {.str="\xEF\xBF\xBD"}},
    { NULL }
};

static void *writer_child_next(void *obj, void *prev)
{
    WriterContext *ctx = obj;
    if (!prev && ctx->writer && ctx->writer->priv_class && ctx->priv)
        return ctx->priv;
    return NULL;
}


static const AVClass writer_class = {
    .class_name = "Writer",
    .item_name  = writer_get_name,
    .option     = writer_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .child_next = writer_child_next,
};

static int writer_close(WriterContext **wctx)
{
    int i;
    int ret = 0;

    if (!*wctx)
        return -1;

    if ((*wctx)->writer->uninit)
        (*wctx)->writer->uninit(*wctx);
    for (i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_finalize(&(*wctx)->section_pbuf[i], NULL);
    if ((*wctx)->writer->priv_class)
        av_opt_free((*wctx)->priv);
    av_freep(&((*wctx)->priv));
    av_opt_free(*wctx);
    if ((*wctx)->avio) {
        avio_flush((*wctx)->avio);
        ret = avio_close((*wctx)->avio);
    }
    av_freep(wctx);
    return ret;
}
static inline int validate_string(WriterContext *wctx, char **dstp, const char *src)
{
    const uint8_t *p, *endp;
    AVBPrint dstbuf;
    int invalid_chars_nb = 0, ret = 0;

    av_bprint_init(&dstbuf, 0, AV_BPRINT_SIZE_UNLIMITED);

    endp = src + strlen(src);
    for (p = (uint8_t *)src; *p;) {
        uint32_t code;
        int invalid = 0;
        const uint8_t *p0 = p;

        if (av_utf8_decode(&code, &p, endp, wctx->string_validation_utf8_flags) < 0) {
            AVBPrint bp;
            av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
            bprint_bytes(&bp, p0, p-p0);
            av_log(wctx, AV_LOG_DEBUG,
                   "Invalid UTF-8 sequence %s found in string '%s'\n", bp.str, src);
            invalid = 1;
        }

        if (invalid) {
            invalid_chars_nb++;

            switch (wctx->string_validation) {
            case WRITER_STRING_VALIDATION_FAIL:
                av_log(wctx, AV_LOG_ERROR,
                       "Invalid UTF-8 sequence found in string '%s'\n", src);
                ret = AVERROR_INVALIDDATA;
                goto end;
                break;

            case WRITER_STRING_VALIDATION_REPLACE:
                av_bprintf(&dstbuf, "%s", wctx->string_validation_replacement);
                break;
            }
        }

        if (!invalid || wctx->string_validation == WRITER_STRING_VALIDATION_IGNORE)
            av_bprint_append_data(&dstbuf, p0, p-p0);
    }

    if (invalid_chars_nb && wctx->string_validation == WRITER_STRING_VALIDATION_REPLACE) {
        av_log(wctx, AV_LOG_WARNING,
               "%d invalid UTF-8 sequence(s) found in string '%s', replaced with '%s'\n",
               invalid_chars_nb, src, wctx->string_validation_replacement);
    }

end:
    av_bprint_finalize(&dstbuf, dstp);
    return ret;
}


#define PRINT_STRING_OPT      1
#define PRINT_STRING_VALIDATE 2

static inline int writer_print_string(WriterContext *wctx,
                                      const char *key, const char *val, int flags)
{
    const struct section *section = wctx->section[wctx->level];
    int ret = 0;

    // if (show_optional_fields == SHOW_OPTIONAL_FIELDS_NEVER ||
    //     (show_optional_fields == SHOW_OPTIONAL_FIELDS_AUTO
    //     && (flags & PRINT_STRING_OPT)
    //     && !(wctx->writer->flags & WRITER_FLAG_DISPLAY_OPTIONAL_FIELDS)))
    //     return 0;

    if (section->show_all_entries || av_dict_get(section->entries_to_show, key, NULL, 0)) {
        if (flags & PRINT_STRING_VALIDATE) {
            char *key1 = NULL, *val1 = NULL;
            ret = validate_string(wctx, &key1, key);
            if (ret < 0) goto end;
            ret = validate_string(wctx, &val1, val);
            if (ret < 0) goto end;
            wctx->writer->print_string(wctx, key1, val1);
        end:
            if (ret < 0) {
                av_log(wctx, AV_LOG_ERROR,
                       "Invalid key=value string combination %s=%s in section %s\n",
                       key, val, section->unique_name);
            }
            av_free(key1);
            av_free(val1);
        } else {
            wctx->writer->print_string(wctx, key, val);
        }

        wctx->nb_item[wctx->level]++;
    }

    return ret;
}

static inline void writer_print_integer(WriterContext *wctx, const char *key, long long int val)
{
    const struct section *section = wctx->section[wctx->level];

    if (section->show_all_entries || av_dict_get(section->entries_to_show, key, NULL, 0)) {
        wctx->writer->print_integer(wctx, key, val);
        wctx->nb_item[wctx->level]++;
    }
}
static inline void writer_print_rational(WriterContext *wctx,
                                         const char *key, AVRational q, char sep)
{
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf, "%d%c%d", q.num, sep, q.den);
    writer_print_string(wctx, key, buf.str, 0);
}

static void writer_print_time(WriterContext *wctx, const char *key,
                              int64_t ts, const AVRational *time_base, int is_duration)
{
    char buf[128];

    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
        writer_print_string(wctx, key, "N/A", PRINT_STRING_OPT);
    } else {
        double d = ts * av_q2d(*time_base);
        struct unit_value uv;
        uv.val.d = d;
        uv.unit = unit_second_str;
        value_string(buf, sizeof(buf), uv);
        writer_print_string(wctx, key, buf, 0);
    }
}

static void writer_print_ts(WriterContext *wctx, const char *key, int64_t ts, int is_duration)
{
    if ((!is_duration && ts == AV_NOPTS_VALUE) || (is_duration && ts == 0)) {
        writer_print_string(wctx, key, "N/A", PRINT_STRING_OPT);
    } else {
        writer_print_integer(wctx, key, ts);
    }
}

static void writer_print_data(WriterContext *wctx, const char *name,
                              const uint8_t *data, int size)
{
    AVBPrint bp;
    int offset = 0, l, i;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "\n");
    while (size) {
        av_bprintf(&bp, "%08x: ", offset);
        l = FFMIN(size, 16);
        for (i = 0; i < l; i++) {
            av_bprintf(&bp, "%02x", data[i]);
            if (i & 1)
                av_bprintf(&bp, " ");
        }
        av_bprint_chars(&bp, ' ', 41 - 2 * i - i / 2);
        for (i = 0; i < l; i++)
            av_bprint_chars(&bp, data[i] - 32U < 95 ? data[i] : '.', 1);
        av_bprintf(&bp, "\n");
        offset += l;
        data   += l;
        size   -= l;
    }
    writer_print_string(wctx, name, bp.str, 0);
    av_bprint_finalize(&bp, NULL);
}

static void writer_print_data_hash(WriterContext *wctx, const char *name,
                                   const uint8_t *data, int size)
{
    char *p, buf[AV_HASH_MAX_SIZE * 2 + 64] = { 0 };

    if (!hash)
        return;
    av_hash_init(hash);
    av_hash_update(hash, data, size);
    snprintf(buf, sizeof(buf), "%s:", av_hash_get_name(hash));
    p = buf + strlen(buf);
    av_hash_final_hex(hash, p, buf + sizeof(buf) - p);
    writer_print_string(wctx, name, buf, 0);
}

static void writer_print_integers(WriterContext *wctx, const char *name,
                                  uint8_t *data, int size, const char *format,
                                  int columns, int bytes, int offset_add)
{
    AVBPrint bp;
    int offset = 0, l, i;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "\n");
    while (size) {
        av_bprintf(&bp, "%08x: ", offset);
        l = FFMIN(size, columns);
        for (i = 0; i < l; i++) {
            if      (bytes == 1) av_bprintf(&bp, format, *data);
            else if (bytes == 2) av_bprintf(&bp, format, AV_RN16(data));
            else if (bytes == 4) av_bprintf(&bp, format, AV_RN32(data));
            data += bytes;
            size --;
        }
        av_bprintf(&bp, "\n");
        offset += offset_add;
    }
    writer_print_string(wctx, name, bp.str, 0);
    av_bprint_finalize(&bp, NULL);
}

#define writer_w8(wctx_, b_) (wctx_)->writer_w8(wctx_, b_)
#define writer_put_str(wctx_, str_) (wctx_)->writer_put_str(wctx_, str_)
#define writer_printf(wctx_, fmt_, ...) (wctx_)->writer_printf(wctx_, fmt_, __VA_ARGS__)

DEFINE_WRITER_CLASS(json);

static av_cold int json_init(WriterContext *wctx)
{
    JSONContext *json = wctx->priv;

    json->item_sep       = json->compact ? ", " : ",\n";
    json->item_start_end = json->compact ? " "  : "\n";

    return 0;
}

static const char *json_escape_str(AVBPrint *dst, const char *src, void *log_ctx)
{
    static const char json_escape[] = {'"', '\\', '\b', '\f', '\n', '\r', '\t', 0};
    static const char json_subst[]  = {'"', '\\',  'b',  'f',  'n',  'r',  't', 0};
    const char *p;

    for (p = src; *p; p++) {
        char *s = strchr(json_escape, *p);
        if (s) {
            av_bprint_chars(dst, '\\', 1);
            av_bprint_chars(dst, json_subst[s - json_escape], 1);
        } else if ((unsigned char)*p < 32) {
            av_bprintf(dst, "\\u00%02x", *p & 0xff);
        } else {
            av_bprint_chars(dst, *p, 1);
        }
    }
    return dst->str;
}

#define JSON_INDENT() writer_printf(wctx, "%*c", json->indent_level * 4, ' ')

static void json_print_section_header(WriterContext *wctx)
{
    JSONContext *json = wctx->priv;
    AVBPrint buf;
    const struct section *section = wctx->section[wctx->level];
    const struct section *parent_section = wctx->level ?
        wctx->section[wctx->level-1] : NULL;

    if (wctx->level && wctx->nb_item[wctx->level-1])
        writer_put_str(wctx, ",\n");

    if (section->flags & SECTION_FLAG_IS_WRAPPER) {
        writer_put_str(wctx, "{\n");
        json->indent_level++;
    } else {
        av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
        json_escape_str(&buf, section->name, wctx);
        JSON_INDENT();

        json->indent_level++;
        if (section->flags & SECTION_FLAG_IS_ARRAY) {
            writer_printf(wctx, "\"%s\": [\n", buf.str);
        } else if (parent_section && !(parent_section->flags & SECTION_FLAG_IS_ARRAY)) {
            writer_printf(wctx, "\"%s\": {%s", buf.str, json->item_start_end);
        } else {
            writer_printf(wctx, "{%s", json->item_start_end);

            /* this is required so the parser can distinguish between packets and frames */
            if (parent_section && parent_section->id == SECTION_ID_PACKETS_AND_FRAMES) {
                if (!json->compact)
                    JSON_INDENT();
                writer_printf(wctx, "\"type\": \"%s\"", section->name);
                wctx->nb_item[wctx->level]++;
            }
        }
        av_bprint_finalize(&buf, NULL);
    }
}

static void json_print_section_footer(WriterContext *wctx)
{
    JSONContext *json = wctx->priv;
    const struct section *section = wctx->section[wctx->level];

    if (wctx->level == 0) {
        json->indent_level--;
        writer_put_str(wctx, "\n}\n");
    } else if (section->flags & SECTION_FLAG_IS_ARRAY) {
        writer_w8(wctx, '\n');
        json->indent_level--;
        JSON_INDENT();
        writer_w8(wctx, ']');
    } else {
        writer_put_str(wctx, json->item_start_end);
        json->indent_level--;
        if (!json->compact)
            JSON_INDENT();
        writer_w8(wctx, '}');
    }
}

static inline void json_print_item_str(WriterContext *wctx,
                                       const char *key, const char *value)
{
    AVBPrint buf;

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(wctx, "\"%s\":", json_escape_str(&buf, key,   wctx));
    av_bprint_clear(&buf);
    writer_printf(wctx, " \"%s\"", json_escape_str(&buf, value, wctx));
    av_bprint_finalize(&buf, NULL);
}

static void json_print_str(WriterContext *wctx, const char *key, const char *value)
{
    JSONContext *json = wctx->priv;
    const struct section *parent_section = wctx->level ?
        wctx->section[wctx->level-1] : NULL;

    if (wctx->nb_item[wctx->level] || (parent_section && parent_section->id == SECTION_ID_PACKETS_AND_FRAMES))
        writer_put_str(wctx, json->item_sep);
    if (!json->compact)
        JSON_INDENT();
    json_print_item_str(wctx, key, value);
}

static void json_print_int(WriterContext *wctx, const char *key, long long int value)
{
    JSONContext *json = wctx->priv;
    const struct section *parent_section = wctx->level ?
        wctx->section[wctx->level-1] : NULL;
    AVBPrint buf;

    if (wctx->nb_item[wctx->level] || (parent_section && parent_section->id == SECTION_ID_PACKETS_AND_FRAMES))
        writer_put_str(wctx, json->item_sep);
    if (!json->compact)
        JSON_INDENT();

    av_bprint_init(&buf, 1, AV_BPRINT_SIZE_UNLIMITED);
    writer_printf(wctx, "\"%s\": %lld", json_escape_str(&buf, key, wctx), value);
    av_bprint_finalize(&buf, NULL);
}

static const Writer json_writer = {
    .name                 = "json",
    .priv_size            = sizeof(JSONContext),
    .init                 = json_init,
    .print_section_header = json_print_section_header,
    .print_section_footer = json_print_section_footer,
    .print_integer        = json_print_int,
    .print_string         = json_print_str,
    .flags = WRITER_FLAG_PUT_PACKETS_AND_FRAMES_IN_SAME_CHAPTER,
    .priv_class           = &json_class,
};



static const Writer *registered_writers[1];
static int writer_register(const Writer *writer)
{
    static int next_registered_writer_idx = 0;
    registered_writers[next_registered_writer_idx++] = writer;
    return 0;
}

static void writer_register_all(void)
{
    static int initialized;

    if (initialized)
        return;
    initialized = 1;
    writer_register(&json_writer);
}

static int writer_open(WriterContext **wctx, const Writer *writer, const char *args,
                       const struct section *sections, int nb_sections, const char *output)
{
    int i, ret = 0;

    if (!(*wctx = av_mallocz(sizeof(WriterContext)))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if (!((*wctx)->priv = av_mallocz(writer->priv_size))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    (*wctx)->class = &writer_class;
    (*wctx)->writer = writer;
    (*wctx)->level = -1;
    (*wctx)->sections = sections;
    (*wctx)->nb_sections = nb_sections;

    av_opt_set_defaults(*wctx);

    if (writer->priv_class) {
        void *priv_ctx = (*wctx)->priv;
        *((const AVClass **)priv_ctx) = writer->priv_class;
        av_opt_set_defaults(priv_ctx);
    }

    /* convert options to dictionary */
    if (args) {
        AVDictionary *opts = NULL;
        const AVDictionaryEntry *opt = NULL;

        if ((ret = av_dict_parse_string(&opts, args, "=", ":", 0)) < 0) {
            av_log(*wctx, AV_LOG_ERROR, "Failed to parse option string '%s' provided to writer context\n", args);
            av_dict_free(&opts);
            goto fail;
        }

        while ((opt = av_dict_get(opts, "", opt, AV_DICT_IGNORE_SUFFIX))) {
            if ((ret = av_opt_set(*wctx, opt->key, opt->value, AV_OPT_SEARCH_CHILDREN)) < 0) {
                av_log(*wctx, AV_LOG_ERROR, "Failed to set option '%s' with value '%s' provided to writer context\n",
                       opt->key, opt->value);
                av_dict_free(&opts);
                goto fail;
            }
        }

        av_dict_free(&opts);
    }

    /* validate replace string */
    {
        const uint8_t *p = (*wctx)->string_validation_replacement;
        const uint8_t *endp = p + strlen(p);
        while (*p) {
            const uint8_t *p0 = p;
            int32_t code;
            ret = av_utf8_decode(&code, &p, endp, (*wctx)->string_validation_utf8_flags);
            if (ret < 0) {
                AVBPrint bp;
                av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
                bprint_bytes(&bp, p0, p-p0),
                    av_log(wctx, AV_LOG_ERROR,
                           "Invalid UTF8 sequence %s found in string validation replace '%s'\n",
                           bp.str, (*wctx)->string_validation_replacement);
                return ret;
            }
        }
    }

    if (!output_filename) {
        (*wctx)->writer_w8 = writer_w8_printf;
        (*wctx)->writer_put_str = writer_put_str_printf;
        (*wctx)->writer_printf = writer_printf_printf;
    } else {
        if ((ret = avio_open(&(*wctx)->avio, output, AVIO_FLAG_WRITE)) < 0) {
            av_log(*wctx, AV_LOG_ERROR,
                   "Failed to open output '%s' with error: %s\n", output, av_err2str(ret));
            goto fail;
        }
        (*wctx)->writer_w8 = writer_w8_avio;
        (*wctx)->writer_put_str = writer_put_str_avio;
        (*wctx)->writer_printf = writer_printf_avio;
    }

    for (i = 0; i < SECTION_MAX_NB_LEVELS; i++)
        av_bprint_init(&(*wctx)->section_pbuf[i], 1, AV_BPRINT_SIZE_UNLIMITED);

    if ((*wctx)->writer->init)
        ret = (*wctx)->writer->init(*wctx);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    writer_close(wctx);
    return ret;
}

static inline void writer_print_section_header(WriterContext *wctx,
                                               int section_id)
{
    int parent_section_id;
    wctx->level++;
    av_assert0(wctx->level < SECTION_MAX_NB_LEVELS);
    parent_section_id = wctx->level ?
        (wctx->section[wctx->level-1])->id : SECTION_ID_NONE;

    wctx->nb_item[wctx->level] = 0;
    wctx->section[wctx->level] = &wctx->sections[section_id];

    if (section_id == SECTION_ID_PACKETS_AND_FRAMES) {
        wctx->nb_section_packet = wctx->nb_section_frame =
        wctx->nb_section_packet_frame = 0;
    } else if (parent_section_id == SECTION_ID_PACKETS_AND_FRAMES) {
        wctx->nb_section_packet_frame = section_id == SECTION_ID_PACKET ?
            wctx->nb_section_packet : wctx->nb_section_frame;
    }

    if (wctx->writer->print_section_header)
        wctx->writer->print_section_header(wctx);
}

static inline void writer_print_section_footer(WriterContext *wctx)
{
    int section_id = wctx->section[wctx->level]->id;
    int parent_section_id = wctx->level ?
        wctx->section[wctx->level-1]->id : SECTION_ID_NONE;

    if (parent_section_id != SECTION_ID_NONE)
        wctx->nb_item[wctx->level-1]++;
    if (parent_section_id == SECTION_ID_PACKETS_AND_FRAMES) {
        if (section_id == SECTION_ID_PACKET) wctx->nb_section_packet++;
        else                                     wctx->nb_section_frame++;
    }
    if (wctx->writer->print_section_footer)
        wctx->writer->print_section_footer(wctx);
    wctx->level--;
}



#define print_fmt(k, f, ...) do {              \
    av_bprint_clear(&pbuf);                    \
    av_bprintf(&pbuf, f, __VA_ARGS__);         \
    writer_print_string(w, k, pbuf.str, 0);    \
} while (0)

#define print_list_fmt(k, f, n, ...) do {       \
    av_bprint_clear(&pbuf);                     \
    for (int idx = 0; idx < n; idx++) {         \
        if (idx > 0)                            \
            av_bprint_chars(&pbuf, ' ', 1);     \
        av_bprintf(&pbuf, f, __VA_ARGS__);      \
    }                                           \
    writer_print_string(w, k, pbuf.str, 0);     \
} while (0)

#define print_int(k, v)         writer_print_integer(w, k, v)
#define print_q(k, v, s)        writer_print_rational(w, k, v, s)
#define print_str(k, v)         writer_print_string(w, k, v, 0)
#define print_str_opt(k, v)     writer_print_string(w, k, v, PRINT_STRING_OPT)
#define print_str_validate(k, v) writer_print_string(w, k, v, PRINT_STRING_VALIDATE)
#define print_time(k, v, tb)    writer_print_time(w, k, v, tb, 0)
#define print_ts(k, v)          writer_print_ts(w, k, v, 0)
#define print_duration_time(k, v, tb) writer_print_time(w, k, v, tb, 1)
#define print_duration_ts(k, v)       writer_print_ts(w, k, v, 1)
#define print_val(k, v, u) do {                                     \
    struct unit_value uv;                                           \
    uv.val.i = v;                                                   \
    uv.unit = u;                                                    \
    writer_print_string(w, k, value_string(val_str, sizeof(val_str), uv), 0); \
} while (0)

#define print_section_header(s) writer_print_section_header(w, s);
#define print_section_footer(s) writer_print_section_footer(w, s);

#define REALLOCZ_ARRAY_STREAM(ptr, cur_n, new_n)                        \
{                                                                       \
    ret = av_reallocp_array(&(ptr), (new_n), sizeof(*(ptr)));           \
    if (ret < 0)                                                        \
        goto end;                                                       \
    memset( (ptr) + (cur_n), 0, ((new_n) - (cur_n)) * sizeof(*(ptr)) ); \
}

static int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec)
{
    int ret = avformat_match_stream_specifier(s, st, spec);
    if (ret < 0)
        av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
    return ret;
}


static AVDictionary *filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id,
                                AVFormatContext *s, AVStream *st, const AVCodec *codec)
{
    AVDictionary    *ret = NULL;
    const AVDictionaryEntry *t = NULL;
    int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                                      : AV_OPT_FLAG_DECODING_PARAM;
    char          prefix = 0;
    const AVClass    *cc = avcodec_get_class();

    if (!codec)
        codec            = s->oformat ? avcodec_find_encoder(codec_id)
                                      : avcodec_find_decoder(codec_id);

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix  = 'v';
        flags  |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix  = 'a';
        flags  |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix  = 's';
        flags  |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    }

    while (t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)) {
        const AVClass *priv_class;
        char *p = strchr(t->key, ':');

        /* check stream specification in opt name */
        if (p)
            switch (check_stream_specifier(s, st, p + 1)) {
            case  1: *p = 0; break;
            case  0:         continue;
            default:         exit_program(1);
            }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            ((priv_class = codec->priv_class) &&
             av_opt_find(&priv_class, t->key, NULL, flags,
                         AV_OPT_SEARCH_FAKE_OBJ)))
            av_dict_set(&ret, t->key, t->value, 0);
        else if (t->key[0] == prefix &&
                 av_opt_find(&cc, t->key + 1, NULL, flags,
                             AV_OPT_SEARCH_FAKE_OBJ))
            av_dict_set(&ret, t->key + 1, t->value, 0);

        if (p)
            *p = ':';
    }
    return ret;
}


static AVDictionary **setup_find_stream_info_opts(AVFormatContext *s,
                                           AVDictionary *codec_opts)
{
    int i;
    AVDictionary **opts;

    if (!s->nb_streams)
        return NULL;
    opts = av_calloc(s->nb_streams, sizeof(*opts));
    if (!opts) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not alloc memory for stream options.\n");
        exit_program(1);
    }
    for (i = 0; i < s->nb_streams; i++)
        opts[i] = filter_codec_opts(codec_opts, s->streams[i]->codecpar->codec_id,
                                    s, s->streams[i], NULL);
    return opts;
}


static inline int show_tags(WriterContext *w, AVDictionary *tags, int section_id)
{
    const AVDictionaryEntry *tag = NULL;
    int ret = 0;

    if (!tags)
        return 0;
    writer_print_section_header(w, section_id);

    while ((tag = av_dict_get(tags, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if ((ret = print_str_validate(tag->key, tag->value)) < 0)
            break;
    }
    writer_print_section_footer(w);

    return ret;
}


static int show_format(WriterContext *w, InputFile *ifile)
{
    AVFormatContext *fmt_ctx = ifile->fmt_ctx;
    char val_str[128];
    int64_t size = fmt_ctx->pb ? avio_size(fmt_ctx->pb) : -1;
    int ret = 0;

    writer_print_section_header(w, SECTION_ID_FORMAT);
    print_str_validate("filename", fmt_ctx->url);
    print_int("nb_streams",       fmt_ctx->nb_streams);
    print_int("nb_programs",      fmt_ctx->nb_programs);
    print_str("format_name",      fmt_ctx->iformat->name);
 
    if (fmt_ctx->iformat->long_name) print_str    ("format_long_name", fmt_ctx->iformat->long_name);
    else                             print_str_opt("format_long_name", "unknown");
    
    print_time("start_time",      fmt_ctx->start_time, &AV_TIME_BASE_Q);
    print_time("duration",        fmt_ctx->duration,   &AV_TIME_BASE_Q);
    if (size >= 0) print_val    ("size", size, unit_byte_str);
    else           print_str_opt("size", "N/A");
    if (fmt_ctx->bit_rate > 0) print_val    ("bit_rate", fmt_ctx->bit_rate, unit_bit_per_second_str);
    else                       print_str_opt("bit_rate", "N/A");
    print_int("probe_score", fmt_ctx->probe_score);
    ret = show_tags(w, fmt_ctx->metadata, SECTION_ID_FORMAT_TAGS);

    writer_print_section_footer(w);
    fflush(stdout);
    return ret;
}


static int open_input_file(InputFile *ifile, const char *filename,
                           const char *print_filename)
{
    int err, i;
    AVFormatContext *fmt_ctx = NULL;
    const AVDictionaryEntry *t = NULL;
    int scan_all_pmts_set = 0;

    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        print_error(filename, AVERROR(ENOMEM));
        exit_program(1);
    }

    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    if ((err = avformat_open_input(&fmt_ctx, filename,
                                   iformat, &format_opts)) < 0) {
        print_error(filename, err);
        return err;
    }
    if (print_filename) {
        av_freep(&fmt_ctx->url);
        fmt_ctx->url = av_strdup(print_filename);
    }
    ifile->fmt_ctx = fmt_ctx;
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    while ((t = av_dict_get(format_opts, "", t, AV_DICT_IGNORE_SUFFIX)))
        av_log(NULL, AV_LOG_WARNING, "Option %s skipped - not known to demuxer.\n", t->key);


    AVDictionary **opts = setup_find_stream_info_opts(fmt_ctx, codec_opts);
    int orig_nb_streams = fmt_ctx->nb_streams;

    err = avformat_find_stream_info(fmt_ctx, opts);

    for (i = 0; i < orig_nb_streams; i++)
        av_dict_free(&opts[i]);
    av_freep(&opts);

    if (err < 0) {
        print_error(filename, err);
        return err;
    }
    

    av_dump_format(fmt_ctx, 0, filename, 0);

    ifile->streams = av_calloc(fmt_ctx->nb_streams, sizeof(*ifile->streams));
    if (!ifile->streams)
        exit(1);
    ifile->nb_streams = fmt_ctx->nb_streams;

    /* bind a decoder to each input stream */
    for (i = 0; i < fmt_ctx->nb_streams; i++) {
        InputStream *ist = &ifile->streams[i];
        AVStream *stream = fmt_ctx->streams[i];
        const AVCodec *codec;

        ist->st = stream;

        if (stream->codecpar->codec_id == AV_CODEC_ID_PROBE) {
            av_log(NULL, AV_LOG_WARNING,
                   "Failed to probe codec for input stream %d\n",
                    stream->index);
            continue;
        }

        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            av_log(NULL, AV_LOG_WARNING,
                    "Unsupported codec with id %d for input stream %d\n",
                    stream->codecpar->codec_id, stream->index);
            continue;
        }
        {
            AVDictionary *opts = filter_codec_opts(codec_opts, stream->codecpar->codec_id,
                                                   fmt_ctx, stream, codec);

            ist->dec_ctx = avcodec_alloc_context3(codec);
            if (!ist->dec_ctx)
                exit(1);

            err = avcodec_parameters_to_context(ist->dec_ctx, stream->codecpar);
            if (err < 0)
                exit(1);

            ist->dec_ctx->pkt_timebase = stream->time_base;

            if (avcodec_open2(ist->dec_ctx, codec, &opts) < 0) {
                av_log(NULL, AV_LOG_WARNING, "Could not open codec for input stream %d\n",
                       stream->index);
                exit(1);
            }

            if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
                av_log(NULL, AV_LOG_ERROR, "Option %s for input stream %d not found\n",
                       t->key, stream->index);
                return AVERROR_OPTION_NOT_FOUND;
            }
        }
    }

    ifile->fmt_ctx = fmt_ctx;
    return 0;
}

static void close_input_file(InputFile *ifile)
{
    int i;

    /* close decoder for each stream */
    for (i = 0; i < ifile->nb_streams; i++)
        avcodec_free_context(&ifile->streams[i].dec_ctx);

    av_freep(&ifile->streams);
    ifile->nb_streams = 0;

    avformat_close_input(&ifile->fmt_ctx);
}

static void print_pkt_side_data(WriterContext *w,
                                AVCodecParameters *par,
                                const AVPacketSideData *side_data,
                                int nb_side_data,
                                SectionID id_data_list,
                                SectionID id_data)
{
    int i;

    writer_print_section_header(w, id_data_list);
    for (i = 0; i < nb_side_data; i++) {
        const AVPacketSideData *sd = &side_data[i];
        const char *name = av_packet_side_data_name(sd->type);

        writer_print_section_header(w, id_data);
        print_str("side_data_type", name ? name : "unknown");
        if (sd->type == AV_PKT_DATA_DISPLAYMATRIX && sd->size >= 9*4) {
            writer_print_integers(w, "displaymatrix", sd->data, 9, " %11d", 3, 4, 1);
            print_int("rotation", av_display_rotation_get((int32_t *)sd->data));
        } else if (sd->type == AV_PKT_DATA_STEREO3D) {
            const AVStereo3D *stereo = (AVStereo3D *)sd->data;
            print_str("type", av_stereo3d_type_name(stereo->type));
            print_int("inverted", !!(stereo->flags & AV_STEREO3D_FLAG_INVERT));
        } else if (sd->type == AV_PKT_DATA_SPHERICAL) {
            const AVSphericalMapping *spherical = (AVSphericalMapping *)sd->data;
            print_str("projection", av_spherical_projection_name(spherical->projection));
            if (spherical->projection == AV_SPHERICAL_CUBEMAP) {
                print_int("padding", spherical->padding);
            } else if (spherical->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE) {
                size_t l, t, r, b;
                av_spherical_tile_bounds(spherical, par->width, par->height,
                                         &l, &t, &r, &b);
                print_int("bound_left", l);
                print_int("bound_top", t);
                print_int("bound_right", r);
                print_int("bound_bottom", b);
            }

            print_int("yaw", (double) spherical->yaw / (1 << 16));
            print_int("pitch", (double) spherical->pitch / (1 << 16));
            print_int("roll", (double) spherical->roll / (1 << 16));
        } else if (sd->type == AV_PKT_DATA_SKIP_SAMPLES && sd->size == 10) {
            print_int("skip_samples",    AV_RL32(sd->data));
            print_int("discard_padding", AV_RL32(sd->data + 4));
            print_int("skip_reason",     AV_RL8(sd->data + 8));
            print_int("discard_reason",  AV_RL8(sd->data + 9));
        } else if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) {
            AVMasteringDisplayMetadata *metadata = (AVMasteringDisplayMetadata *)sd->data;

            if (metadata->has_primaries) {
                print_q("red_x", metadata->display_primaries[0][0], '/');
                print_q("red_y", metadata->display_primaries[0][1], '/');
                print_q("green_x", metadata->display_primaries[1][0], '/');
                print_q("green_y", metadata->display_primaries[1][1], '/');
                print_q("blue_x", metadata->display_primaries[2][0], '/');
                print_q("blue_y", metadata->display_primaries[2][1], '/');

                print_q("white_point_x", metadata->white_point[0], '/');
                print_q("white_point_y", metadata->white_point[1], '/');
            }

            if (metadata->has_luminance) {
                print_q("min_luminance", metadata->min_luminance, '/');
                print_q("max_luminance", metadata->max_luminance, '/');
            }
        } else if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
            AVContentLightMetadata *metadata = (AVContentLightMetadata *)sd->data;
            print_int("max_content", metadata->MaxCLL);
            print_int("max_average", metadata->MaxFALL);
        } else if (sd->type == AV_PKT_DATA_DOVI_CONF) {
            AVDOVIDecoderConfigurationRecord *dovi = (AVDOVIDecoderConfigurationRecord *)sd->data;
            print_int("dv_version_major", dovi->dv_version_major);
            print_int("dv_version_minor", dovi->dv_version_minor);
            print_int("dv_profile", dovi->dv_profile);
            print_int("dv_level", dovi->dv_level);
            print_int("rpu_present_flag", dovi->rpu_present_flag);
            print_int("el_present_flag", dovi->el_present_flag);
            print_int("bl_present_flag", dovi->bl_present_flag);
            print_int("dv_bl_signal_compatibility_id", dovi->dv_bl_signal_compatibility_id);
        } else if (sd->type == AV_PKT_DATA_AUDIO_SERVICE_TYPE) {
            enum AVAudioServiceType *t = (enum AVAudioServiceType *)sd->data;
            print_int("service_type", *t);
        } else if (sd->type == AV_PKT_DATA_MPEGTS_STREAM_ID) {
            print_int("id", *sd->data);
        } else if (sd->type == AV_PKT_DATA_CPB_PROPERTIES) {
            const AVCPBProperties *prop = (AVCPBProperties *)sd->data;
            print_int("max_bitrate", prop->max_bitrate);
            print_int("min_bitrate", prop->min_bitrate);
            print_int("avg_bitrate", prop->avg_bitrate);
            print_int("buffer_size", prop->buffer_size);
            print_int("vbv_delay",   prop->vbv_delay);
        } else if (sd->type == AV_PKT_DATA_WEBVTT_IDENTIFIER ||
                   sd->type == AV_PKT_DATA_WEBVTT_SETTINGS) {
            // if (do_show_data)
            //     writer_print_data(w, "data", sd->data, sd->size);
            writer_print_data_hash(w, "data_hash", sd->data, sd->size);
        } else if (sd->type == AV_PKT_DATA_AFD && sd->size > 0) {
            print_int("active_format", *sd->data);
        }
        writer_print_section_footer(w);
    }
    writer_print_section_footer(w);
}


static void print_color_range(WriterContext *w, enum AVColorRange color_range)
{
    const char *val = av_color_range_name(color_range);
    if (!val || color_range == AVCOL_RANGE_UNSPECIFIED) {
        print_str_opt("color_range", "unknown");
    } else {
        print_str("color_range", val);
    }
}

static void print_color_space(WriterContext *w, enum AVColorSpace color_space)
{
    const char *val = av_color_space_name(color_space);
    if (!val || color_space == AVCOL_SPC_UNSPECIFIED) {
        print_str_opt("color_space", "unknown");
    } else {
        print_str("color_space", val);
    }
}

static void print_primaries(WriterContext *w, enum AVColorPrimaries color_primaries)
{
    const char *val = av_color_primaries_name(color_primaries);
    if (!val || color_primaries == AVCOL_PRI_UNSPECIFIED) {
        print_str_opt("color_primaries", "unknown");
    } else {
        print_str("color_primaries", val);
    }
}

static void print_color_trc(WriterContext *w, enum AVColorTransferCharacteristic color_trc)
{
    const char *val = av_color_transfer_name(color_trc);
    if (!val || color_trc == AVCOL_TRC_UNSPECIFIED) {
        print_str_opt("color_transfer", "unknown");
    } else {
        print_str("color_transfer", val);
    }
}

static void print_chroma_location(WriterContext *w, enum AVChromaLocation chroma_location)
{
    const char *val = av_chroma_location_name(chroma_location);
    if (!val || chroma_location == AVCHROMA_LOC_UNSPECIFIED) {
        print_str_opt("chroma_location", "unspecified");
    } else {
        print_str("chroma_location", val);
    }
}


static int show_stream(WriterContext *w, AVFormatContext *fmt_ctx, int stream_idx, InputStream *ist)
{
    AVStream *stream = ist->st;
    AVCodecParameters *par;
    AVCodecContext *dec_ctx;
    char val_str[128];
    const char *s;
    AVRational sar, dar;
    AVBPrint pbuf;
    const AVCodecDescriptor *cd;
    int ret = 0;
    const char *profile = NULL;

    av_bprint_init(&pbuf, 1, AV_BPRINT_SIZE_UNLIMITED);

    writer_print_section_header(w, SECTION_ID_STREAM);

    print_int("index", stream->index);

    par     = stream->codecpar;
    dec_ctx = ist->dec_ctx;
    if (cd = avcodec_descriptor_get(par->codec_id)) {
        print_str("codec_name", cd->name);
        print_str("codec_long_name",
                      cd->long_name ? cd->long_name : "unknown");
        
    } else {
        print_str_opt("codec_name", "unknown");
        print_str_opt("codec_long_name", "unknown");
    }

    if (profile = avcodec_profile_name(par->codec_id, par->profile))
        print_str("profile", profile);
    else {
        if (par->profile != FF_PROFILE_UNKNOWN) {
            char profile_num[12];
            snprintf(profile_num, sizeof(profile_num), "%d", par->profile);
            print_str("profile", profile_num);
        } else
            print_str_opt("profile", "unknown");
    }

    s = av_get_media_type_string(par->codec_type);
    if (s) print_str    ("codec_type", s);
    else   print_str_opt("codec_type", "unknown");

    /* print AVI/FourCC tag */
    print_str("codec_tag_string",    av_fourcc2str(par->codec_tag));
    print_fmt("codec_tag", "0x%04"PRIx32, par->codec_tag);

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        print_int("width",        par->width);
        print_int("height",       par->height);
        if (dec_ctx) {
            print_int("coded_width",  dec_ctx->coded_width);
            print_int("coded_height", dec_ctx->coded_height);
            print_int("closed_captions", !!(dec_ctx->properties & FF_CODEC_PROPERTY_CLOSED_CAPTIONS));
            print_int("film_grain", !!(dec_ctx->properties & FF_CODEC_PROPERTY_FILM_GRAIN));
        }
        print_int("has_b_frames", par->video_delay);
        sar = av_guess_sample_aspect_ratio(fmt_ctx, stream, NULL);
        if (sar.num) {
            print_q("sample_aspect_ratio", sar, ':');
            av_reduce(&dar.num, &dar.den,
                      par->width  * sar.num,
                      par->height * sar.den,
                      1024*1024);
            print_q("display_aspect_ratio", dar, ':');
        } else {
            print_str_opt("sample_aspect_ratio", "N/A");
            print_str_opt("display_aspect_ratio", "N/A");
        }
        s = av_get_pix_fmt_name(par->format);
        if (s) print_str    ("pix_fmt", s);
        else   print_str_opt("pix_fmt", "unknown");
        print_int("level",   par->level);

        print_color_range(w, par->color_range);
        print_color_space(w, par->color_space);
        print_color_trc(w, par->color_trc);
        print_primaries(w, par->color_primaries);
        print_chroma_location(w, par->chroma_location);

        if (par->field_order == AV_FIELD_PROGRESSIVE)
            print_str("field_order", "progressive");
        else if (par->field_order == AV_FIELD_TT)
            print_str("field_order", "tt");
        else if (par->field_order == AV_FIELD_BB)
            print_str("field_order", "bb");
        else if (par->field_order == AV_FIELD_TB)
            print_str("field_order", "tb");
        else if (par->field_order == AV_FIELD_BT)
            print_str("field_order", "bt");
        else
            print_str_opt("field_order", "unknown");

        if (dec_ctx)
            print_int("refs", dec_ctx->refs);
        break;

    case AVMEDIA_TYPE_AUDIO:
        s = av_get_sample_fmt_name(par->format);
        if (s) print_str    ("sample_fmt", s);
        else   print_str_opt("sample_fmt", "unknown");
        print_val("sample_rate",     par->sample_rate, unit_hertz_str);
        print_int("channels",        par->ch_layout.nb_channels);

        if (par->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_describe(&par->ch_layout, val_str, sizeof(val_str));
            print_str    ("channel_layout", val_str);
        } else {
            print_str_opt("channel_layout", "unknown");
        }

        print_int("bits_per_sample", av_get_bits_per_sample(par->codec_id));
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        if (par->width)
            print_int("width",       par->width);
        else
            print_str_opt("width",   "N/A");
        if (par->height)
            print_int("height",      par->height);
        else
            print_str_opt("height",  "N/A");
        break;
    }

    // if (dec_ctx && dec_ctx->codec->priv_class && show_private_data) {
    //     const AVOption *opt = NULL;
    //     while (opt = av_opt_next(dec_ctx->priv_data,opt)) {
    //         uint8_t *str;
    //         if (!(opt->flags & AV_OPT_FLAG_EXPORT)) continue;
    //         if (av_opt_get(dec_ctx->priv_data, opt->name, 0, &str) >= 0) {
    //             print_str(opt->name, str);
    //             av_free(str);
    //         }
    //     }
    // }

    if (fmt_ctx->iformat->flags & AVFMT_SHOW_IDS) print_fmt    ("id", "0x%x", stream->id);
    else                                          print_str_opt("id", "N/A");
    print_q("r_frame_rate",   stream->r_frame_rate,   '/');
    print_q("avg_frame_rate", stream->avg_frame_rate, '/');
    print_q("time_base",      stream->time_base,      '/');
    print_ts  ("start_pts",   stream->start_time);
    print_time("start_time",  stream->start_time, &stream->time_base);
    print_ts  ("duration_ts", stream->duration);
    print_time("duration",    stream->duration, &stream->time_base);
    if (par->bit_rate > 0)     print_val    ("bit_rate", par->bit_rate, unit_bit_per_second_str);
    else                       print_str_opt("bit_rate", "N/A");
    if (dec_ctx && dec_ctx->rc_max_rate > 0)
        print_val ("max_bit_rate", dec_ctx->rc_max_rate, unit_bit_per_second_str);
    else
        print_str_opt("max_bit_rate", "N/A");
    if (dec_ctx && dec_ctx->bits_per_raw_sample > 0) print_fmt("bits_per_raw_sample", "%d", dec_ctx->bits_per_raw_sample);
    else                                             print_str_opt("bits_per_raw_sample", "N/A");
    if (stream->nb_frames) print_fmt    ("nb_frames", "%"PRId64, stream->nb_frames);
    else                   print_str_opt("nb_frames", "N/A");
    if (nb_streams_frames[stream_idx])  print_fmt    ("nb_read_frames", "%"PRIu64, nb_streams_frames[stream_idx]);
    else                                print_str_opt("nb_read_frames", "N/A");
    if (nb_streams_packets[stream_idx]) print_fmt    ("nb_read_packets", "%"PRIu64, nb_streams_packets[stream_idx]);
    else                                print_str_opt("nb_read_packets", "N/A");
    // if (do_show_data)
    //     writer_print_data(w, "extradata", par->extradata,
    //                                       par->extradata_size);

    if (par->extradata_size > 0) {
        print_int("extradata_size", par->extradata_size);
        writer_print_data_hash(w, "extradata_hash", par->extradata,
                                                    par->extradata_size);
    }

    /* Print disposition information */
#define PRINT_DISPOSITION(flagname, name) do {                                \
        print_int(name, !!(stream->disposition & AV_DISPOSITION_##flagname)); \
    } while (0)

    writer_print_section_header(w, SECTION_ID_STREAM_DISPOSITION);
    PRINT_DISPOSITION(DEFAULT,          "default");
    PRINT_DISPOSITION(DUB,              "dub");
    PRINT_DISPOSITION(ORIGINAL,         "original");
    PRINT_DISPOSITION(COMMENT,          "comment");
    PRINT_DISPOSITION(LYRICS,           "lyrics");
    PRINT_DISPOSITION(KARAOKE,          "karaoke");
    PRINT_DISPOSITION(FORCED,           "forced");
    PRINT_DISPOSITION(HEARING_IMPAIRED, "hearing_impaired");
    PRINT_DISPOSITION(VISUAL_IMPAIRED,  "visual_impaired");
    PRINT_DISPOSITION(CLEAN_EFFECTS,    "clean_effects");
    PRINT_DISPOSITION(ATTACHED_PIC,     "attached_pic");
    PRINT_DISPOSITION(TIMED_THUMBNAILS, "timed_thumbnails");
    PRINT_DISPOSITION(CAPTIONS,         "captions");
    PRINT_DISPOSITION(DESCRIPTIONS,     "descriptions");
    PRINT_DISPOSITION(METADATA,         "metadata");
    PRINT_DISPOSITION(DEPENDENT,        "dependent");
    PRINT_DISPOSITION(STILL_IMAGE,      "still_image");
    writer_print_section_footer(w);
    

    ret = show_tags(w, stream->metadata, SECTION_ID_STREAM_TAGS);

    if (stream->nb_side_data) {
        print_pkt_side_data(w, stream->codecpar, stream->side_data, stream->nb_side_data,
                            SECTION_ID_STREAM_SIDE_DATA_LIST,
                            SECTION_ID_STREAM_SIDE_DATA);
    }

    writer_print_section_footer(w);
    av_bprint_finalize(&pbuf, NULL);
    fflush(stdout);

    return ret;
}

static int show_streams(WriterContext *w, InputFile *ifile)
{
    AVFormatContext *fmt_ctx = ifile->fmt_ctx;
    int i, ret = 0;

    writer_print_section_header(w, SECTION_ID_STREAMS);
    for (i = 0; i < ifile->nb_streams; i++)
        if (selected_streams[i]) {
            ret = show_stream(w, fmt_ctx, i, &ifile->streams[i]);
            if (ret < 0)
                break;
        }
    writer_print_section_footer(w);

    return ret;
}


static int probe_file(WriterContext *wctx, const char *filename,
                      const char *print_filename)
{
    InputFile ifile = { 0 };
    int ret, i;
    int section_id;

    ret = open_input_file(&ifile, filename, print_filename);
    if (ret < 0)
        goto end;

#define CHECK_END if (ret < 0) goto end

    nb_streams = ifile.fmt_ctx->nb_streams;
    REALLOCZ_ARRAY_STREAM(nb_streams_frames,0,ifile.fmt_ctx->nb_streams);
    REALLOCZ_ARRAY_STREAM(nb_streams_packets,0,ifile.fmt_ctx->nb_streams);
    REALLOCZ_ARRAY_STREAM(selected_streams,0,ifile.fmt_ctx->nb_streams);

    for (i = 0; i < ifile.fmt_ctx->nb_streams; i++) {
        selected_streams[i] = 1;
        // if (!selected_streams[i])
        //     ifile.fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
    }

    ret = show_streams(wctx, &ifile);
    CHECK_END;
    
    
    ret = show_format(wctx, &ifile);
    CHECK_END;
    
end:
    if (ifile.fmt_ctx)
        close_input_file(&ifile);
    av_freep(&nb_streams_frames);
    av_freep(&nb_streams_packets);
    av_freep(&selected_streams);

    return ret;
}



int ffprobe(int argc, char **argv) {
  if (argc < 3) {
    return 1;
  }
  register_exit(ffprobe_cleanup);

  const Writer *w;
  WriterContext *wctx;
  char *buf;
  char *w_args = NULL;
  int ret, input_ret, i;
  // json writter
  writer_register_all();
  input_filename = argv[1];
  output_filename = argv[2];

  // printf("input: %s, output: %s", input_filename, output_filename);
  w = registered_writers[0];
  if ((ret = writer_open(&wctx, w, w_args,
                        sections, FF_ARRAY_ELEMS(sections), output_filename)) >= 0) {
    writer_print_section_header(wctx, SECTION_ID_ROOT);
    ret = probe_file(wctx, input_filename, print_input_filename);
  }

  input_ret = ret;
  writer_print_section_footer(wctx);
  ret = writer_close(&wctx);
  if (ret < 0)
    av_log(NULL, AV_LOG_ERROR, "Writing output failed: %s\n", av_err2str(ret));

  ret = FFMIN(ret, input_ret);

end:
    av_hash_freep(&hash);
    uninit_opts();
    for (i = 0; i < FF_ARRAY_ELEMS(sections); i++)
        av_dict_free(&(sections[i].entries_to_show));
  return ret < 0;
}

// int main(int argc, char **argv) {
//   if (argc < 3) {
//     printf("usage: %s output_file\n"
//             "API example program to output a media file with libavformat.\n"
//             "This program generates a synthetic audio and video stream, encodes and\n"
//             "muxes them into a file named output_file.\n"
//             "The output format is automatically guessed according to the file extension.\n"
//             "Raw images can also be output by using '%%d' in the filename.\n"
//             "\n", argv[0]);
//     return 1;
//   }

//   int ret = ffprobe(argv[1], argv[2]);
//   return ret;
// }