#include "ruby/ruby.h"
#include "ruby/debug.h"
#include "rbkit_stack_trace.h"

#define BUF_SIZE 2048

// Struct named `statics` contain the static variables used in this file.
// The values of this struct are initialized here.
static struct _stack_trace_statics {
  rbkit_map_t *stacktrace_map;
  st_table *stacktrace_strings_stacktrace_ids_table;
} statics = {
  .stacktrace_map = NULL,
  .stacktrace_strings_stacktrace_ids_table = NULL
};

static char* concat(char *s1, char *s2) {
  size_t len1 = strlen(s1);
  size_t len2 = strlen(s2);
  char *result = malloc(len1+len2+1);
  memcpy(result, s1, len1);
  memcpy(result+len1, s2, len2+1);//+1 to copy the null-terminator
  return result;
}

static int compare_strings(char *s1, char *s2) {
  if(s1 && s2) {
    return (strcmp(s1, s2) == 0);
  } else {
    // True if both are NULL
    return (s1 == s2);
  }
}

static int same_stack_trace(rbkit_stack_trace *trace1, rbkit_stack_trace *trace2) {
  size_t i;
  if(trace1->frame_count != trace2->frame_count) {
    return 0;
  }
  for (i = 0; i < trace1->frame_count; ++i) {
    rbkit_frame_data frame1 = trace1->frames[i];
    rbkit_frame_data frame2 = trace2->frames[i];

    if(
        !compare_strings(frame1.method_name, frame2.method_name) ||
        !compare_strings(frame1.label, frame2.label) ||
        !compare_strings(frame1.file, frame2.file) ||
        (frame1.line != frame2.line) ||
        (frame1.is_singleton_method != frame2.is_singleton_method)
      ) {
      return 0;
    }
  }
  return 1;
}

typedef struct _find_stacktrace_arg {
  rbkit_stack_trace *existing_stack_trace;
  rbkit_stack_trace *new_stack_trace;
} find_stacktrace_arg;

static int find_stacktrace_i(st_data_t key, st_data_t value, st_data_t a) {
  find_stacktrace_arg *arg = (find_stacktrace_arg *)a;
  rbkit_stack_trace *stacktrace = (rbkit_stack_trace *)key;
  if(same_stack_trace(stacktrace, arg->new_stack_trace)) {
    arg->existing_stack_trace = stacktrace;
    return ST_STOP;
  }
  return ST_CONTINUE;
}

static rbkit_stack_trace *find_or_create_stack_trace_entry(st_table *stacktraces_with_same_methods, rbkit_stack_trace *stacktrace) {
  size_t n;
  rbkit_stack_trace *return_stacktrace = stacktrace;
  st_table *table = stacktraces_with_same_methods;
  find_stacktrace_arg *arg = malloc(sizeof(find_stacktrace_arg));
  arg->existing_stack_trace = NULL;
  arg->new_stack_trace = stacktrace;

  st_foreach(stacktraces_with_same_methods, find_stacktrace_i, (st_data_t)arg);
  if(arg->existing_stack_trace) {
    delete_stack_trace(stacktrace);
    return_stacktrace = arg->existing_stack_trace;
  }

  if (st_lookup(table, (st_data_t)return_stacktrace, &n)) {
    st_insert(table, (st_data_t)stacktrace, n+1);
  } else {
    st_add_direct(table, (st_data_t)return_stacktrace, 1);
  }
  return return_stacktrace;
}

static void increment_stack_trace_count(rbkit_stack_trace *stacktrace) {
  st_table *table = statics.stacktrace_map->table;
  size_t n;

  if (st_lookup(table, (st_data_t)stacktrace, &n)) {
    st_insert(table, (st_data_t)stacktrace, n+1);
  }
  else {
    st_add_direct(table, (st_data_t)stacktrace, 1);
    statics.stacktrace_map->count++;
  }
}

static rbkit_stack_trace * record_stack_trace(rbkit_stack_trace *stacktrace, char *signature) {
  st_table *stacktraces_with_same_methods;
  rbkit_stack_trace *existing_stack_trace = NULL;
  if (st_lookup(statics.stacktrace_strings_stacktrace_ids_table, (st_data_t)signature, (st_data_t *)&stacktraces_with_same_methods)) {
    existing_stack_trace = find_or_create_stack_trace_entry(stacktraces_with_same_methods, stacktrace);
  } else {
    stacktraces_with_same_methods = st_init_numtable();
    existing_stack_trace = stacktrace;
    st_add_direct(stacktraces_with_same_methods, (st_data_t)stacktrace, 1);
    st_add_direct(statics.stacktrace_strings_stacktrace_ids_table, (st_data_t)signature, (st_data_t)stacktraces_with_same_methods);
  }
  increment_stack_trace_count(existing_stack_trace);
  return existing_stack_trace;
}

static char * copy_rb_string(VALUE source) {
  char *temp_string = StringValueCStr(source);
  char *dest = malloc(strlen(temp_string) + 1);
  snprintf(dest, strlen(temp_string) + 1, "%s", temp_string);
  return dest;
}

rbkit_stack_trace *collect_stack_trace() {
  int start = 0;
  int lines[BUF_SIZE];
  VALUE buff[BUF_SIZE];
  VALUE rb_method_name, rb_label, rb_file, rb_singleton_method;
  int i, is_singleton;
  char *tmp;

  char *method_name, *label, *file;
  char *method_names_concatenated = malloc(0);
  unsigned long line;// thread_id;

  int collected_size = rb_profile_frames(start, sizeof(buff) / sizeof(VALUE), buff, lines);
  rbkit_frame_data *frame_data = malloc(sizeof(rbkit_frame_data) * collected_size);
  rbkit_stack_trace *stacktrace = malloc(sizeof(rbkit_stack_trace));
  stacktrace->frames = frame_data;
  stacktrace->frame_count = collected_size;

  rb_gc_disable();
  for (i=0; i<collected_size; i++) {
    rb_method_name = rb_profile_frame_method_name(buff[i]);
    if(NIL_P(rb_method_name)) {
      method_name = NULL;
    } else {
      method_name = copy_rb_string(rb_method_name);
      tmp = concat(method_names_concatenated, method_name);
      free(method_names_concatenated);
      method_names_concatenated = tmp;
    }
    frame_data[i].method_name = method_name;

    rb_label = rb_profile_frame_full_label(buff[i]);
    if(NIL_P(rb_label)) {
      label = NULL;
    } else {
      label = copy_rb_string(rb_label);
      frame_data[i].label = label;
    }

    rb_file = rb_profile_frame_absolute_path(buff[i]);
    if(NIL_P(rb_file))
      rb_file = rb_profile_frame_path(buff[i]);
    file = StringValueCStr(rb_file);
    file = copy_rb_string(rb_file);
    frame_data[i].file = file;

    line = FIX2ULONG(rb_profile_frame_first_lineno(buff[i]));
    frame_data[i].line = line;

    rb_singleton_method = rb_profile_frame_singleton_method_p(buff[i]);
    is_singleton = rb_singleton_method == Qtrue;
    frame_data[i].is_singleton_method = is_singleton;

    /*thread_id = FIX2ULONG(rb_obj_id(rb_thread_current()));*/
    /*frame_data[i].thread_id = thread_id;*/
  }
  rb_gc_enable();
  return record_stack_trace(stacktrace, method_names_concatenated);
}

void delete_stack_trace(rbkit_stack_trace *stacktrace) {
  size_t count;
  for(count = 0; count < stacktrace->frame_count; count++){
    free(stacktrace->frames[count].method_name);
    free(stacktrace->frames[count].label);
    free(stacktrace->frames[count].file);
  }
  free(stacktrace->frames);
  free(stacktrace);
}

void init_stack_trace_maps() {
  statics.stacktrace_map = malloc(sizeof(rbkit_map_t));
  statics.stacktrace_map->table = st_init_numtable();
  statics.stacktrace_map->count = 0;
  statics.stacktrace_strings_stacktrace_ids_table = st_init_strtable();
}

rbkit_map_t *get_stack_traces() {
  return statics.stacktrace_map;
}

static int delete_stack_trace_i(st_data_t key, st_data_t value, st_data_t arg) {
  rbkit_stack_trace *stacktrace = (rbkit_stack_trace *)key;
  delete_stack_trace(stacktrace);
  return ST_DELETE;
}

static int delete_stack_trace_strings_i(st_data_t key, st_data_t value, st_data_t arg) {
  char *str = (char *)key;
  free(str);
  return ST_DELETE;
}

void clear_stack_traces() {
  statics.stacktrace_map->count = 0;
  st_foreach(statics.stacktrace_map->table, delete_stack_trace_i, 0);
  st_foreach(statics.stacktrace_strings_stacktrace_ids_table, delete_stack_trace_strings_i, 0);
}
