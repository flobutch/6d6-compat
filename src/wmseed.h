#ifndef WMSEED_INCLUDE
#define WMSEED_INCLUDE

#include <stdint.h>
#include <stdio.h>
#include "miniseed.h"
#include "tai.h"
#include "samplebuffer.h"

typedef struct {
  int64_t cut;
  char *template;
  char *station, *location, *channel, *network;
  double sample_rate;
  Samplebuffer *sb;
  int record_number;
  Time record_time;
  MiniSeedRecord record[1];
  int data_pending;
  FILE *output;
  Time last_t;
  int64_t last_sn;
} WMSeed;

WMSeed *wmseed_new(const char *template, const char *station, const char *location, const char *channel, const char *network, double sample_rate, int64_t cut);
int wmseed_destroy(WMSeed *w);
int wmseed_sample(WMSeed *w, int32_t sample);
int wmseed_time(WMSeed *w, Time t, int64_t sample_number);

#endif

#ifdef WMSEED_IMPLEMENTATION
#undef WMSEED_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

static void *wmseed__allocate(void *x, size_t size)
{
  void *t;
  if (size) {
    t = realloc(x, size);
    if (!t) {
      fprintf(stderr, "Out of memory!\n");
      fflush(stderr);
      exit(1);
    }
    return t;
  } else {
    free(x);
    return 0;
  }
}

static char *wmseed__strdup(const char *s)
{
  char *s2;
  if (s) {
    s2 = wmseed__allocate(0, strlen(s) + 1);
    strcpy(s2, s);
    return s2;
  } else {
    return 0;
  }
}

#if __GNUC__ || __clang__
__attribute__((format(printf, 2, 3)))
#endif
static char *wmseed__strappend(char *s, const char *format, ...)
{
  size_t l1 = s ? strlen(s) : 0, l2;
  va_list args;
  va_start(args, format);
  l2 = vsnprintf(0, 0, format, args);
  va_end(args);
  s = wmseed__allocate(s, l1 + l2 + 1);
  va_start(args, format);
  vsnprintf(s + l1, l2 + 1, format, args);
  va_end(args);
  return s;
}

static char *wmseed__filename(WMSeed *w, Time t)
{
  int yday;
  Date d = tai_date(t, &yday, 0);
  char *s = 0;
  char *tmpl = w->template;
  while (*tmpl) {
    if (*tmpl == '%') {
      ++tmpl;
      switch (*tmpl) {
      case '%':
        s = wmseed__strappend(s, "%%");
        break;
      case 'y':
        s = wmseed__strappend(s, "%04d", d.year);
        break;
      case 'm':
        s = wmseed__strappend(s, "%02d", d.month);
        break;
      case 'd':
        s = wmseed__strappend(s, "%02d", d.day);
        break;
      case 'h':
        s = wmseed__strappend(s, "%02d", d.hour);
        break;
      case 'i':
        s = wmseed__strappend(s, "%02d", d.min);
        break;
      case 's':
        s = wmseed__strappend(s, "%02d", d.sec);
        break;
      case 'j':
        s = wmseed__strappend(s, "%03d", yday);
        break;
      case 'S':
        s = wmseed__strappend(s, "%s", w->station);
        break;
      case 'L':
        s = wmseed__strappend(s, "%s", w->location);
        break;
      case 'C':
        s = wmseed__strappend(s, "%s", w->channel);
        break;
      case 'N':
        s = wmseed__strappend(s, "%s", w->network);
        break;
      default:
        free(s);
        return 0;
      }
    } else {
      s = wmseed__strappend(s, "%c", *tmpl);
    }
    ++tmpl;
  }
  return s;
}

static char *wmseed__dirname(const char *path)
{
  size_t l = 0, x = -1;
  char *s;
  while (path[l]) {
    if (path[l] == '/') x = l;
    ++l;
  }
  if (x > 1) {
    s = wmseed__allocate(0, x + 1);
    memcpy(s, path, x);
    s[x] = 0;
  } else {
    s = wmseed__allocate(0, 2);
    s[0] = x ? '.' : '/';
    s[1] = 0;
  }
  return s;
}

static int wmseed__mkdir_p(const char *path)
{
  struct stat sb;
  char *p = wmseed__strdup(path);
  size_t i, len = strlen(p);

  // Remove trailing slash.
  if (p[len - 1] == '/') {
    p[len - 1] = 0;
  }

  for (i = 1; i < len; ++i) {
    if (p[i] == '/') {
      p[i] = 0;
      if (stat(p, &sb)) {
        if (mkdir(p, 0755)) {
          free(p);
          return -1;
        }
      } else if (!S_ISDIR(sb.st_mode)) {
        free(p);
        return -1;
      }
      p[i] = '/';
    }
  }
  if (stat(p, &sb)) {
    if (mkdir(p, 0755)) {
      free(p);
      return -1;
    }
  } else if (!S_ISDIR(sb.st_mode)) {
    free(p);
    return -1;
  }
  free(p);
  return 0;
}

WMSeed *wmseed_new(const char *template, const char *station, const char *location, const char *channel, const char *network, double sample_rate, int64_t cut)
{
  WMSeed *w;
  w = wmseed__allocate(0, sizeof(*w));
  w->cut = cut * 1000000;
  w->template = wmseed__strdup(template);
  w->station = wmseed__strdup(station);
  w->location = wmseed__strdup(location);
  w->channel = wmseed__strdup(channel);
  w->network = wmseed__strdup(network);
  w->sample_rate = sample_rate;
  w->sb = samplebuffer_new();
  w->record_number = 0;
  w->record_time = 0;
  w->data_pending = 0;
  w->output = 0;
  w->last_t = 0;
  w->last_sn = -1;
  return w;
}

int wmseed_destroy(WMSeed *w)
{
  if (!w) return -1;
  // Write remaining data.
  // TODO
  // Close files.
  if (w->output) fclose(w->output);
  // Free everything.
  free(w->template);
  free(w->station);
  free(w->location);
  free(w->channel);
  free(w->network);
  samplebuffer_destroy(w->sb);
  free(w);
  return 0;
}

int wmseed_sample(WMSeed *w, int32_t sample)
{
  if (!w || w->last_sn < 0) return -1;
  samplebuffer_push(w->sb, sample);
  return 0;
}

static void wmseed__flush(WMSeed *w)
{
  if (w->data_pending) {
    if (fwrite(w->record->data, sizeof(w->record->data), 1, w->output) != 1) {
      fprintf(stderr, "I/O error!\n");
      fflush(stderr);
      exit(1);
    }
    w->data_pending = 0;
  }
}

static void wmseed__new_record(WMSeed *w, Time t)
{
  Date d;
  if (w->data_pending) {
    if (tai_utc_diff(w->record_time) != tai_utc_diff(t)) {
      miniseed_record_set_leapsec(w->record, 1);
    }
    wmseed__flush(w);
  }
  w->record_number += 1;
  w->record_time = t;
  w->data_pending = 0;
  miniseed_record_init(w->record, w->record_number);
  miniseed_record_set_info(w->record, w->station, w->location, w->channel, w->network);
  miniseed_record_set_sample_rate(w->record, w->sample_rate);
  d = tai_date(t, 0, 0);
  miniseed_record_set_start_time(w->record, d.year, d.month, d.day, d.hour, d.min, d.sec, d.usec / 100, 0);
}

static void wmseed__create_file(WMSeed *w, Time t)
{
  char *filename, *dirname;
  w->record_number = 0;
  wmseed__new_record(w, t);
  if (w->output) {
    fclose(w->output);
  }
  filename = wmseed__filename(w, t);
  dirname = wmseed__dirname(filename);
  wmseed__mkdir_p(dirname);
  free(dirname);
  w->output = fopen(filename, "wb");
  if (!w->output) {
    fprintf(stderr, "Could not create file '%s': %s.\n", filename, strerror(errno));
    fflush(stderr);
    exit(1);
  } else {
    fprintf(stderr, "Created file '%s'.\n", filename);
  }
  free(filename);

}

// b must be positive.
#define wmseed__div(a, b) ((a) / (b) - ((a) % (b) < 0))

int wmseed_time(WMSeed *w, Time t, int64_t sample_number)
{
  int64_t split = -1, off;
  int32_t sample;
  double a;
  Time tt, split_time = 0;
  if (!w) return -1;
  if (w->last_sn == -1) {
    if (sample_number != 0) return -1;
    w->last_t = t;
    w->last_sn = sample_number;
    // Create the first file.
    wmseed__create_file(w, t);
    return 0;
  } else if (sample_number <= w->last_sn || t <= w->last_t) {
    return -1;
  }

  if (sample_number - w->last_sn < 1008 * 20) {
    // Don't use too many timestamps.
    return 0;
  }

  // Use linear interpolation.
  a = (double) (t - w->last_t) / (sample_number - w->last_sn);

  // Check for a cut between the two timestamps.
  // Calculate UTC offset to cut at round UTC dates and times.
  off = 1000000 * tai_utc_diff(t);
  if (w->cut && wmseed__div(w->last_t - off, w->cut) != wmseed__div(t - off, w->cut)) {
    split_time = wmseed__div(t - off, w->cut) * w->cut + off;
    split = w->last_sn + (int64_t) ceil((split_time - w->last_t) / a);
    //printf("%lld\n", (long long) split);
    //fflush(stdout);
  }

  while (w->sb->len && w->sb->sample_number <= sample_number) {
    if (w->sb->sample_number == split) {
      tt = w->last_t + (int64_t) round((w->sb->sample_number - w->last_sn) * a);
      wmseed__create_file(w, tt);
    }
    sample = samplebuffer_pop(w->sb);
    while (miniseed_record_push_sample(w->record, sample) == -1) {
      tt = w->last_t + (int64_t) round((w->sb->sample_number - w->last_sn - 1) * a);
      wmseed__new_record(w, tt);
    }
    w->data_pending = 1;
  }

  // Update state.
  w->last_t = t;
  w->last_sn = sample_number;

  return 0;
}

#endif