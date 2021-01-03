// MIT License

// Copyright (c) 2016-2020 Luis Lloret

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdexcept>
#include <chrono>
#include <iostream>
#include <atomic>
#include "sp_midi.h"
#include "hotplug_thread.h"
#include "scheduler_callback_thread.h"
#include "midiout.h"
#include "midiin.h"
#include "midisendprocessor.h"
#include "version.h"
#include "utils.h"
#include "monitorlogger.h"

static int g_monitor_level = 6;

using namespace std;

// FIXME: need to test what happens when MIDI devices are already in use by another application
// and sp_midi cannot open them
// MIDI out
std::unique_ptr<MidiSendProcessor> midiSendProcessor;

// MIDI in
vector<unique_ptr<MidiIn> > midiInputs;

HotPlugThread *hotplug_thread = nullptr;

SchedulerCallbackThread *scheduler_callback_thread = nullptr;

static ErlNifPid midi_process_pid;

static atomic<bool> g_already_initialized(false);

void prepareMidiSendProcessorOutputs(unique_ptr<MidiSendProcessor>& midiSendProcessor)
{
    // Open all MIDI devices. This is what Sonic Pi does
    vector<string> midiOutputsToOpen = MidiOut::getNonRtMidiOutputNames();
    {
        midiSendProcessor->prepareOutputs(midiOutputsToOpen);
    }
}


void prepareMidiInputs(vector<unique_ptr<MidiIn> >& midiInputs)
{
    // Should we open all devices, or just the ones passed as parameters?
    vector<string> midiInputsToOpen = MidiIn::getNonRtMidiInputNames();

    midiInputs.clear();
    for (const auto& input : midiInputsToOpen) {
        try {
            auto midiInput = make_unique<MidiIn>(input, false);
            midiInputs.push_back(std::move(midiInput));
        } catch (const RtMidiError& e) {
            cout << "Could not open input device " << input << ": " << e.what() << endl;
            //throw;
        }
    }
}


struct timestamp {
    char type;
    int id;
    long long t;
};

vector<timestamp> timestamps;

void print_time_stamp(char type)
{
    static int id_A = 0;
    static int id_B = 0;
    static int id_C = 0;
    auto now = chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    timestamp ts{type, (type == 'A' ? id_A++ : type == 'B'? id_B++ : id_C++), micros};
    timestamps.push_back(ts);
}

void output_time_stamps()
{
    for (auto ts : timestamps) {
        cout << ts.type << "," << ts.id << "," << ts.t << endl;
    }
}


int sp_midi_send(const char* device_name, const unsigned char* c_message, unsigned int size)
{
    midiSendProcessor->addMessage(device_name, c_message, size);

    return 0;
}

int sp_midi_init()
{
    if (g_already_initialized){
        return 0;
    }
    g_already_initialized = true;
    MonitorLogger::getInstance().setLogLevel(g_monitor_level);

    midiSendProcessor = make_unique<MidiSendProcessor>();
    // Prepare the MIDI outputs
    try {
        prepareMidiSendProcessorOutputs(midiSendProcessor);
    } catch (const std::out_of_range&) {
        cout << "Error opening MIDI outputs" << endl;
        return -1;
    }

    // Prepare the MIDI inputs
    try{
        prepareMidiInputs(midiInputs);
    } catch (const std::out_of_range&) {
        cout << "Error opening MIDI inputs" << endl;
        return -1;
    }

    midiSendProcessor->startThread();

    scheduler_callback_thread = new SchedulerCallbackThread;
    scheduler_callback_thread->startThread();

    hotplug_thread = new HotPlugThread;
    hotplug_thread->startThread();

    return 0;
}

void sp_midi_deinit()
{
    if (!g_already_initialized){
        return;
    }
    g_already_initialized = false;
    //output_time_stamps();

    // We tell the threads that we are going to exit. We need to do it this way because there is no MessageManager
    midiSendProcessor->signalThreadShouldExit();
    hotplug_thread->signalThreadShouldExit();
    scheduler_callback_thread->signalThreadShouldExit();

    // We give them some time to exit
    juce::Thread::sleep(1000);

    // And we stop them
    midiSendProcessor->stopThread(0);
    midiInputs.clear();
    midiSendProcessor.reset(nullptr);

    hotplug_thread->stopThread(0);
    delete hotplug_thread;

    scheduler_callback_thread->stopThread(0);
    delete scheduler_callback_thread;

    juce::DeletedAtShutdown::deleteAll();
}

static char **vector_str_to_c(const vector<string>& vector_str)
{
    char **c_str_list;

    c_str_list = (char **)malloc(vector_str.size() * sizeof(char*));
    for (int i = 0; i < vector_str.size(); i++) {
        c_str_list[i] = (char*)malloc((vector_str[i].size() + 1) * sizeof(char));
        strcpy(c_str_list[i], vector_str[i].c_str());
    }

    return c_str_list;
}

char **sp_midi_outs(int *n_list)
{
    auto outputs = MidiOut::getNonRtMidiOutputNames();
    char **c_str_list = vector_str_to_c(outputs);
    *n_list = (int)outputs.size();
    return c_str_list;
}

char **sp_midi_ins(int *n_list)
{
    auto inputs = MidiIn::getNonRtMidiInputNames();
    char **c_str_list = vector_str_to_c(inputs);
    *n_list = (int)inputs.size();
    return c_str_list;
}

long long sp_midi_get_current_time_microseconds()
{
    auto now = chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    long long micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    return micros;
}


// NIF helper functions
ERL_NIF_TERM c_str_list_to_erlang(ErlNifEnv *env, int n, char **c_str_list)
{
    ERL_NIF_TERM *terms = (ERL_NIF_TERM*)malloc(n * sizeof(ERL_NIF_TERM));
    for (int i = 0; i < n; i++) {
        terms[i] = enif_make_string(env, c_str_list[i], ERL_NIF_LATIN1);
    }

    ERL_NIF_TERM string_array = enif_make_list_from_array(env, terms, n);

    for (int i = 0; i < n; i++) {
        free(c_str_list[i]);
    }
    free(c_str_list);
    free(terms);

    return string_array;
}


// NIF functions
ERL_NIF_TERM sp_midi_init_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int ret = sp_midi_init();
    return enif_make_atom(env, "ok");
}

ERL_NIF_TERM sp_midi_deinit_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    sp_midi_deinit();
    return enif_make_atom(env, "ok");
}

ERL_NIF_TERM sp_midi_send_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary bin;
    char device_name[256];

    int ret = enif_get_string(env, argv[0], device_name, 256, ERL_NIF_LATIN1);
    if (!ret){
        return enif_make_badarg(env);
    }

    ret = enif_inspect_binary(env, argv[1], &bin);
    if (!ret){
        return enif_make_badarg(env);
    }

    const unsigned char *c_message = bin.data;
    int size = (int)bin.size;

    int rc = sp_midi_send(device_name, c_message, size);
    if (rc != 0){
        return enif_make_atom(env, "warning");
    }
    return enif_make_atom(env, "ok");
}

ERL_NIF_TERM sp_midi_flush_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    midiSendProcessor->flushMessages();
    return enif_make_atom(env, "ok");
}


ERL_NIF_TERM sp_midi_outs_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int n_midi_outs;
    char **midi_outs = sp_midi_outs(&n_midi_outs);
    return c_str_list_to_erlang(env, n_midi_outs, midi_outs);
}

ERL_NIF_TERM sp_midi_ins_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int n_midi_ins;
    char **midi_ins = sp_midi_ins(&n_midi_ins);
    return c_str_list_to_erlang(env, n_midi_ins, midi_ins);
}

ERL_NIF_TERM sp_midi_have_my_pid_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    if (!enif_self(env, &midi_process_pid)){
        return enif_make_badarg(env);
    }
    return enif_make_atom(env, "ok");
}


ERL_NIF_TERM sp_midi_set_this_pid_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    if (!enif_is_pid(env, argv[0])){
        return enif_make_badarg(env);
    }

    int rc = enif_get_local_pid(env, argv[0], &midi_process_pid);
    return enif_make_atom(env, (rc ? "ok" : "error"));
}


ERL_NIF_TERM sp_midi_set_log_level_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int rc = enif_get_int(env, argv[0], &g_monitor_level);
    MonitorLogger::getInstance().setLogLevel(g_monitor_level);
    return enif_make_atom(env, (rc ? "ok" : "error"));
}


ERL_NIF_TERM sp_midi_get_current_time_microseconds_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    return enif_make_int64(env, sp_midi_get_current_time_microseconds());
}

int send_midi_osc_to_erlang(const char *device_name, const unsigned char *data, size_t size)
{
    ErlNifEnv *msg_env = enif_alloc_env();
    ERL_NIF_TERM term1;
    ERL_NIF_TERM term2;
    ERL_NIF_TERM term3;
    ERL_NIF_TERM term4;

    term1 = enif_make_atom(msg_env, "midi_in");
    term2 = enif_make_string(msg_env, device_name, ERL_NIF_LATIN1);
    unsigned char *term_bin = enif_make_new_binary(msg_env, size, &term3);
    memcpy(term_bin, data, size);

    term4 = enif_make_tuple3(msg_env, term1, term2, term3);
    int rc = enif_send(NULL, &midi_process_pid, msg_env, term4);
    enif_free_env(msg_env);
    return rc;
}


ERL_NIF_TERM sp_midi_schedule_callback_nif(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifSInt64 time_to_trigger;
    ErlNifPid pid;
    ErlNifSInt64 integer;

    if (!enif_get_int64(env, argv[0], &time_to_trigger)){
        return enif_make_badarg(env);
    }
    if (!enif_is_pid(env, argv[1])){
        return enif_make_badarg(env);
    }
    if (!enif_get_local_pid(env, argv[1], &pid)){
        return enif_make_badarg(env);
    }
    if (!enif_get_int64(env, argv[2], &integer)){
        return enif_make_badarg(env);
    }

    scheduler_callback_thread->trigger_callback_at(time_to_trigger, pid, integer);
    return enif_make_atom(env, "ok");
}


static ErlNifFunc nif_funcs[] = {
    {"midi_init", 0, sp_midi_init_nif},
    {"midi_deinit", 0, sp_midi_deinit_nif},
    {"midi_send", 2, sp_midi_send_nif},
    {"midi_flush", 0, sp_midi_flush_nif},
    {"midi_outs", 0, sp_midi_outs_nif},
    {"midi_ins", 0, sp_midi_ins_nif},
    {"have_my_pid", 0, sp_midi_have_my_pid_nif},
    {"set_this_pid", 1, sp_midi_set_this_pid_nif},
    {"set_log_level", 1, sp_midi_set_log_level_nif},
    {"schedule_callback", 3, sp_midi_schedule_callback_nif},
    {"get_current_time_microseconds", 0, sp_midi_get_current_time_microseconds_nif}
};

ERL_NIF_INIT(sp_midi, nif_funcs, NULL, NULL, NULL, NULL);
