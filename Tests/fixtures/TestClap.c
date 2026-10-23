/*
    Two small CLAP plug-ins for MixerTest, so the CLAP host is checked on every platform:

      tf.test.half   an audio effect: stereo in and out, one parameter "Gain" (0..1, default 0.5)
                     that scales the input, with its value kept in the plug-in's saved state.
      tf.test.tone   an instrument that takes notes in CLAP's own note events only (no MIDI),
                     and plays a sine at each note's pitch while it is held.
*/
#include <clap/clap.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
 #define EXPORT __declspec(dllexport)
#else
 #define EXPORT __attribute__((visibility("default")))
#endif

typedef struct
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    int kind;              /* 0 half, 1 tone */
    double gain;           /* half */
    double rate;
    int key;               /* tone: the held note, or -1 */
    double velocity, phase;
} test_plugin;

/* ---- shared ---- */
static bool tp_init (const clap_plugin_t* p) { (void) p; return true; }
static void tp_destroy (const clap_plugin_t* p) { free ((void*) p->plugin_data); }
static bool tp_activate (const clap_plugin_t* p, double rate, uint32_t minF, uint32_t maxF)
{
    (void) minF; (void) maxF;
    ((test_plugin*) p->plugin_data)->rate = rate;
    return true;
}
static void tp_deactivate (const clap_plugin_t* p) { (void) p; }
static bool tp_start (const clap_plugin_t* p) { (void) p; return true; }
static void tp_stop (const clap_plugin_t* p) { (void) p; }
static void tp_reset (const clap_plugin_t* p) { (void) p; }
static void tp_main (const clap_plugin_t* p) { (void) p; }

static void handle_event (test_plugin* t, const clap_event_header_t* e)
{
    if (e->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (e->type == CLAP_EVENT_PARAM_VALUE)
    {
        const clap_event_param_value_t* v = (const clap_event_param_value_t*) e;
        if (v->param_id == 1) t->gain = v->value;
    }
    else if (e->type == CLAP_EVENT_NOTE_ON)
    {
        const clap_event_note_t* n = (const clap_event_note_t*) e;
        t->key = n->key;
        t->velocity = n->velocity;
    }
    else if (e->type == CLAP_EVENT_NOTE_OFF)
    {
        const clap_event_note_t* n = (const clap_event_note_t*) e;
        if (n->key == t->key) t->key = -1;
    }
}

static clap_process_status tp_process (const clap_plugin_t* p, const clap_process_t* proc)
{
    test_plugin* t = (test_plugin*) p->plugin_data;
    const uint32_t events = proc->in_events->size (proc->in_events);
    uint32_t next = 0;
    for (uint32_t i = 0; i < proc->frames_count; ++i)
    {
        while (next < events)
        {
            const clap_event_header_t* e = proc->in_events->get (proc->in_events, next);
            if (e->time > i) break;
            handle_event (t, e);
            ++next;
        }
        float l = 0, r = 0;
        if (t->kind == 0)
        {
            l = proc->audio_inputs[0].data32[0][i] * (float) t->gain;
            r = proc->audio_inputs[0].data32[1][i] * (float) t->gain;
        }
        else if (t->key >= 0)
        {
            const double f = 440.0 * pow (2.0, (t->key - 69) / 12.0);
            l = r = (float) (0.5 * t->velocity * sin (t->phase));
            t->phase += 2.0 * 3.14159265358979 * f / t->rate;
        }
        proc->audio_outputs[0].data32[0][i] = l;
        proc->audio_outputs[0].data32[1][i] = r;
    }
    return CLAP_PROCESS_CONTINUE;
}

/* ---- audio ports ---- */
static uint32_t ap_count (const clap_plugin_t* p, bool input)
{
    const test_plugin* t = (const test_plugin*) p->plugin_data;
    return input && t->kind == 1 ? 0 : 1;
}
static bool ap_get (const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info)
{
    (void) p;
    if (index != 0) return false;
    memset (info, 0, sizeof (*info));
    info->id = input ? 0 : 1;
    strcpy (info->name, input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t audio_ports = { ap_count, ap_get };

/* ---- note ports (tone only): CLAP note events, no MIDI ---- */
static uint32_t np_count (const clap_plugin_t* p, bool input) { (void) p; return input ? 1 : 0; }
static bool np_get (const clap_plugin_t* p, uint32_t index, bool input, clap_note_port_info_t* info)
{
    (void) p;
    if (index != 0 || ! input) return false;
    memset (info, 0, sizeof (*info));
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    strcpy (info->name, "Notes");
    return true;
}
static const clap_plugin_note_ports_t note_ports = { np_count, np_get };

/* ---- params (half only) ---- */
static uint32_t pr_count (const clap_plugin_t* p) { (void) p; return 1; }
static bool pr_info (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    (void) p;
    if (index != 0) return false;
    memset (info, 0, sizeof (*info));
    info->id = 1;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    strcpy (info->name, "Gain");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.5;
    return true;
}
static bool pr_value (const clap_plugin_t* p, clap_id id, double* out)
{
    if (id != 1) return false;
    *out = ((test_plugin*) p->plugin_data)->gain;
    return true;
}
static bool pr_to_text (const clap_plugin_t* p, clap_id id, double v, char* out, uint32_t size)
{
    (void) p; (void) id;
    if (size < 16) return false;
    out[0] = (char) ('0' + (int) (v * 9.99));
    strcpy (out + 1, "/10");
    return true;
}
static bool pr_from_text (const clap_plugin_t* p, clap_id id, const char* text, double* out)
{
    (void) p; (void) id;
    *out = atof (text);
    return true;
}
static void pr_flush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
{
    (void) out;
    for (uint32_t i = 0; i < in->size (in); ++i)
        handle_event ((test_plugin*) p->plugin_data, in->get (in, i));
}
static const clap_plugin_params_t params = { pr_count, pr_info, pr_value, pr_to_text, pr_from_text, pr_flush };

/* ---- state (half only) ---- */
static bool st_save (const clap_plugin_t* p, const clap_ostream_t* s)
{
    const double g = ((test_plugin*) p->plugin_data)->gain;
    return s->write (s, &g, sizeof (g)) == (int64_t) sizeof (g);
}
static bool st_load (const clap_plugin_t* p, const clap_istream_t* s)
{
    double g = 0;
    if (s->read (s, &g, sizeof (g)) != (int64_t) sizeof (g)) return false;
    ((test_plugin*) p->plugin_data)->gain = g;
    return true;
}
static const clap_plugin_state_t state = { st_save, st_load };

static const void* tp_extension (const clap_plugin_t* p, const char* id)
{
    const test_plugin* t = (const test_plugin*) p->plugin_data;
    if (strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &audio_ports;
    if (t->kind == 1 && strcmp (id, CLAP_EXT_NOTE_PORTS) == 0) return &note_ports;
    if (t->kind == 0 && strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (t->kind == 0 && strcmp (id, CLAP_EXT_STATE) == 0) return &state;
    return NULL;
}

/* ---- descriptors and factory ---- */
static const char* half_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };
static const char* tone_features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, NULL };
static const clap_plugin_descriptor_t descriptors[2] = {
    { CLAP_VERSION_INIT, "tf.test.half", "TF Test Half", "78Hz", "", "", "", "1.0", "Halves its input", half_features },
    { CLAP_VERSION_INIT, "tf.test.tone", "TF Test Tone", "78Hz", "", "", "", "1.0", "A sine per note", tone_features } };

static uint32_t f_count (const clap_plugin_factory_t* f) { (void) f; return 2; }
static const clap_plugin_descriptor_t* f_descriptor (const clap_plugin_factory_t* f, uint32_t i) { (void) f; return i < 2 ? &descriptors[i] : NULL; }
static const clap_plugin_t* f_create (const clap_plugin_factory_t* f, const clap_host_t* host, const char* id)
{
    (void) f;
    int kind = strcmp (id, "tf.test.half") == 0 ? 0 : strcmp (id, "tf.test.tone") == 0 ? 1 : -1;
    if (kind < 0) return NULL;
    test_plugin* t = (test_plugin*) calloc (1, sizeof (test_plugin));
    t->host = host;
    t->kind = kind;
    t->gain = 0.5;
    t->key = -1;
    t->rate = 48000;
    t->plugin.desc = &descriptors[kind];
    t->plugin.plugin_data = t;
    t->plugin.init = tp_init;
    t->plugin.destroy = tp_destroy;
    t->plugin.activate = tp_activate;
    t->plugin.deactivate = tp_deactivate;
    t->plugin.start_processing = tp_start;
    t->plugin.stop_processing = tp_stop;
    t->plugin.reset = tp_reset;
    t->plugin.process = tp_process;
    t->plugin.get_extension = tp_extension;
    t->plugin.on_main_thread = tp_main;
    return &t->plugin;
}
static const clap_plugin_factory_t factory = { f_count, f_descriptor, f_create };

static bool e_init (const char* path) { (void) path; return true; }
static void e_deinit (void) {}
static const void* e_factory (const char* id) { return strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
