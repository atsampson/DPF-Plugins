/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2021 Filipe Coelho <falktx@falktx.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "DistrhoUIInternal.hpp"

#include "travesty/edit_controller.h"
#include "travesty/host.h"
#include "travesty/view.h"

#ifdef DISTRHO_PROPER_CPP11_SUPPORT
# include <atomic>
#else
// quick and dirty std::atomic replacement for the things we need
namespace std {
    struct atomic_int {
        volatile int value;
        explicit atomic_int(volatile int v) noexcept : value(v) {}
        int operator++() volatile noexcept { return __atomic_add_fetch(&value, 1, __ATOMIC_RELAXED); }
        int operator--() volatile noexcept { return __atomic_sub_fetch(&value, 1, __ATOMIC_RELAXED); }
        operator int() volatile noexcept { return __atomic_load_n(&value, __ATOMIC_RELAXED); }
    };
};
#endif

/* TODO items:
 * - mousewheel event
 * - key down/up events
 * - size constraints
 * - host-side resize
 * - oddities with init and size callback (triggered too early?)
 */

#if !(defined(DISTRHO_OS_MAC) || defined(DISTRHO_OS_WINDOWS))
# define DPF_VST3_USING_HOST_RUN_LOOP
#endif

#ifndef DPF_VST3_TIMER_INTERVAL
# define DPF_VST3_TIMER_INTERVAL 16 /* ~60 fps */
#endif

START_NAMESPACE_DISTRHO

// --------------------------------------------------------------------------------------------------------------------

#if ! DISTRHO_PLUGIN_WANT_MIDI_INPUT
static constexpr const sendNoteFunc sendNoteCallback = nullptr;
#endif
#if ! DISTRHO_PLUGIN_WANT_STATE
static constexpr const setStateFunc setStateCallback = nullptr;
#endif

// --------------------------------------------------------------------------------------------------------------------
// Utility functions (defined on plugin side)

const char* tuid2str(const v3_tuid iid);
void strncpy_utf16(int16_t* dst, const char* src, size_t length);

struct ScopedUTF16String {
    int16_t* str;
    ScopedUTF16String(const char* const s) noexcept;
    ~ScopedUTF16String() noexcept;
    operator int16_t*() const noexcept;
};

// --------------------------------------------------------------------------------------------------------------------

/**
 * VST3 UI class.
 *
 * All the dynamic things from VST3 get implemented here, free of complex low-level VST3 pointer things.
 * The UI is created during the "attach" view event, and destroyed during "removed".
 *
 * Note that DPF VST3 implementation works over the connection point interface,
 * rather than using edit controller directly.
 * This allows the UI to be running remotely from the DSP.
 *
 * The low-level VST3 stuff comes after.
 */
class UIVst3
#if defined(DISTRHO_OS_MAC) || defined(DISTRHO_OS_WINDOWS)
  : public IdleCallback
#endif
{
public:
    UIVst3(v3_plugin_view** const view,
           v3_host_application** const host,
           v3_connection_point** const connection,
           v3_plugin_frame** const frame,
           const intptr_t winId,
           const float scaleFactor,
           const double sampleRate,
           void* const instancePointer)
        : fView(view),
          fHostContext(host),
          fConnection(connection),
          fFrame(frame),
          fReadyForPluginData(false),
          fScaleFactor(scaleFactor),
          fUI(this, winId, sampleRate,
              editParameterCallback,
              setParameterCallback,
              setStateCallback,
              sendNoteCallback,
              setSizeCallback,
              nullptr, // TODO file request
              nullptr, // bundlePath
              instancePointer,
              scaleFactor)
    {
#if defined(DISTRHO_OS_MAC) || defined(DISTRHO_OS_WINDOWS)
        fUI.addIdleCallbackForVST3(this, DPF_VST3_TIMER_INTERVAL);
#endif
    }

    ~UIVst3()
    {
#if defined(DISTRHO_OS_MAC) || defined(DISTRHO_OS_WINDOWS)
        fUI.removeIdleCallbackForVST3(this);
#endif
        if (fConnection != nullptr)
            disconnect();
    }

    void reconnectIfNeeded()
    {
        if (fConnection != nullptr)
            connect(fConnection);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_plugin_view interface calls

    v3_result onWheel(float /*distance*/)
    {
        // TODO
        return V3_NOT_IMPLEMENTED;
    }

    v3_result onKeyDown(int16_t /*key_char*/, int16_t /*key_code*/, int16_t /*modifiers*/)
    {
        // TODO
        return V3_NOT_IMPLEMENTED;
    }

    v3_result onKeyUp(int16_t /*key_char*/, int16_t /*key_code*/, int16_t /*modifiers*/)
    {
        // TODO
        return V3_NOT_IMPLEMENTED;
    }

    v3_result getSize(v3_view_rect* const rect) const noexcept
    {
        std::memset(rect, 0, sizeof(v3_view_rect));

        rect->right = fUI.getWidth();
        rect->bottom = fUI.getHeight();
#ifdef DISTRHO_OS_MAC
        const double scaleFactor = fUI.getScaleFactor();
        rect->right /= scaleFactor;
        rect->bottom /= scaleFactor;
#endif

        return V3_OK;
    }

    v3_result onSize(v3_view_rect* const /*rect*/)
    {
        // TODO
        return V3_NOT_IMPLEMENTED;
    }

    v3_result onFocus(const bool state)
    {
#if !DISTRHO_PLUGIN_HAS_EXTERNAL_UI
        fUI.notifyFocusChanged(state);
        return V3_OK;
#else
        return V3_NOT_IMPLEMENTED;
        // unused
        (void)state;
#endif
    }

    v3_result setFrame(v3_plugin_frame** const frame) noexcept
    {
        fFrame = frame;
        return V3_OK;
    }

    v3_result checkSizeConstraint(v3_view_rect* const /*rect*/)
    {
        // TODO
        return V3_NOT_IMPLEMENTED;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_connection_point interface calls

    void connect(v3_connection_point** const point) noexcept
    {
        DISTRHO_SAFE_ASSERT_RETURN(point != nullptr,);

        fConnection = point;

        d_stdout("requesting current plugin state");

        v3_message** const message = createMessage("init");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);
    }

    void disconnect() noexcept
    {
        DISTRHO_SAFE_ASSERT_RETURN(fConnection != nullptr,);

        d_stdout("reporting UI closed");
        fReadyForPluginData = false;

        v3_message** const message = createMessage("close");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);

        fConnection = nullptr;
    }

    v3_result notify(v3_message** const message)
    {
        const char* const msgid = v3_cpp_obj(message)->get_message_id(message);
        DISTRHO_SAFE_ASSERT_RETURN(msgid != nullptr, V3_INVALID_ARG);

        v3_attribute_list** const attrs = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrs != nullptr, V3_INVALID_ARG);

        if (std::strcmp(msgid, "ready") == 0)
        {
            DISTRHO_SAFE_ASSERT_RETURN(! fReadyForPluginData, V3_INTERNAL_ERR);
            fReadyForPluginData = true;
            return V3_OK;
        }

        if (std::strcmp(msgid, "parameter-set") == 0)
        {
            int64_t rindex;
            double value;
            v3_result res;

            res = v3_cpp_obj(attrs)->get_int(attrs, "rindex", &rindex);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);

            res = v3_cpp_obj(attrs)->get_float(attrs, "value", &value);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);

#if DISTRHO_PLUGIN_WANT_PROGRAMS
            if (rindex == 0)
            {
                DISTRHO_SAFE_ASSERT_RETURN(value >= 0.0, V3_INTERNAL_ERR);

                fUI.programLoaded(static_cast<uint32_t>(value + 0.5));
            }
            else
#endif
            {
                rindex -= fUI.getParameterOffset();
                DISTRHO_SAFE_ASSERT_RETURN(rindex >= 0, V3_INTERNAL_ERR);

                fUI.parameterChanged(static_cast<uint32_t>(rindex), value);
            }

            return V3_OK;
        }

#if DISTRHO_PLUGIN_WANT_STATE
        if (std::strcmp(msgid, "state-set") == 0)
        {
            int64_t keyLength = -1;
            int64_t valueLength = -1;
            v3_result res;

            res = v3_cpp_obj(attrs)->get_int(attrs, "key:length", &keyLength);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);
            DISTRHO_SAFE_ASSERT_INT_RETURN(keyLength >= 0, keyLength, V3_INTERNAL_ERR);

            res = v3_cpp_obj(attrs)->get_int(attrs, "value:length", &valueLength);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);
            DISTRHO_SAFE_ASSERT_INT_RETURN(valueLength >= 0, valueLength, V3_INTERNAL_ERR);

            int16_t* const key16 = (int16_t*)std::malloc(sizeof(int16_t)*(keyLength + 1));
            DISTRHO_SAFE_ASSERT_RETURN(key16 != nullptr, V3_NOMEM);

            int16_t* const value16 = (int16_t*)std::malloc(sizeof(int16_t)*(valueLength + 1));
            DISTRHO_SAFE_ASSERT_RETURN(value16 != nullptr, V3_NOMEM);

            res = v3_cpp_obj(attrs)->get_string(attrs, "key", key16, sizeof(int16_t)*keyLength);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);

            res = v3_cpp_obj(attrs)->get_string(attrs, "value", value16, sizeof(int16_t)*valueLength);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);

            // do cheap inline conversion
            char* const key = (char*)key16;
            char* const value = (char*)value16;

            for (int64_t i=0; i<keyLength; ++i)
                key[i] = key16[i];
            for (int64_t i=0; i<valueLength; ++i)
                value[i] = value16[i];

            key[keyLength] = '\0';
            value[valueLength] = '\0';

            fUI.stateChanged(key, value);

            std::free(key16);
            std::free(value16);
            return V3_OK;
        }
#endif

        if (std::strcmp(msgid, "sample-rate") == 0)
        {
            double sampleRate;
            v3_result res;

            res = v3_cpp_obj(attrs)->get_float(attrs, "value", &sampleRate);
            DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_OK, res, res);
            DISTRHO_SAFE_ASSERT_RETURN(sampleRate > 0, V3_INVALID_ARG);

            fUI.setSampleRate(sampleRate, true);
            return V3_OK;
        }

        d_stdout("UIVst3 received unknown msg '%s'", msgid);

        return V3_NOT_IMPLEMENTED;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_plugin_view_content_scale_steinberg interface calls

    v3_result setContentScaleFactor(const float factor)
    {
        if (d_isEqual(fScaleFactor, factor))
            return V3_OK;

        fScaleFactor = factor;
        fUI.notifyScaleFactorChanged(factor);
        return V3_OK;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // special idle callback on macOS and Windows

#if defined(DISTRHO_OS_MAC) || defined(DISTRHO_OS_WINDOWS)
    void idleCallback() override
    {
        if (fReadyForPluginData)
        {
            fReadyForPluginData = false;
            requestMorePluginData();
        }

        fUI.idleForVST3();
    }
#else
    // ----------------------------------------------------------------------------------------------------------------
    // v3_timer_handler interface calls

    void onTimer()
    {
        if (fReadyForPluginData)
        {
            fReadyForPluginData = false;
            requestMorePluginData();
        }

        fUI.plugin_idle();
    }
#endif

    // ----------------------------------------------------------------------------------------------------------------

private:
    // VST3 stuff
    v3_plugin_view** const fView;
    v3_host_application** const fHostContext;
    v3_connection_point** fConnection;
    v3_plugin_frame** fFrame;

    // Temporary data
    bool fReadyForPluginData;
    float fScaleFactor;

    // Plugin UI (after VST3 stuff so the UI can call into us during its constructor)
    UIExporter fUI;

    // ----------------------------------------------------------------------------------------------------------------
    // helper functions called during message passing

    v3_message** createMessage(const char* const id) const
    {
        DISTRHO_SAFE_ASSERT_RETURN(fHostContext != nullptr, nullptr);

        v3_tuid iid;
        memcpy(iid, v3_message_iid, sizeof(v3_tuid));
        v3_message** msg = nullptr;
        const v3_result res = v3_cpp_obj(fHostContext)->create_instance(fHostContext, iid, iid, (void**)&msg);
        DISTRHO_SAFE_ASSERT_INT_RETURN(res == V3_TRUE, res, nullptr);
        DISTRHO_SAFE_ASSERT_RETURN(msg != nullptr, nullptr);

        v3_cpp_obj(msg)->set_message_id(msg, id);
        return msg;
    }

    void requestMorePluginData() const
    {
        DISTRHO_SAFE_ASSERT_RETURN(fConnection != nullptr,);

        v3_message** const message = createMessage("idle");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // DPF callbacks

    void editParameter(const uint32_t rindex, const bool started) const
    {
        DISTRHO_SAFE_ASSERT_RETURN(fConnection != nullptr,);

        v3_message** const message = createMessage("parameter-edit");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(attrlist)->set_int(attrlist, "rindex", rindex);
        v3_cpp_obj(attrlist)->set_int(attrlist, "started", started ? 1 : 0);
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);
    }

    static void editParameterCallback(void* ptr, uint32_t rindex, bool started)
    {
        ((UIVst3*)ptr)->editParameter(rindex, started);
    }

    void setParameterValue(const uint32_t rindex, const float realValue)
    {
        DISTRHO_SAFE_ASSERT_RETURN(fConnection != nullptr,);

        v3_message** const message = createMessage("parameter-set");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(attrlist)->set_int(attrlist, "rindex", rindex);
        v3_cpp_obj(attrlist)->set_float(attrlist, "value", realValue);
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);
    }

    static void setParameterCallback(void* ptr, uint32_t rindex, float value)
    {
        ((UIVst3*)ptr)->setParameterValue(rindex, value);
    }

    void setSize(uint width, uint height)
    {
        DISTRHO_SAFE_ASSERT_RETURN(fView != nullptr,);
        DISTRHO_SAFE_ASSERT_RETURN(fFrame != nullptr,);
        d_stdout("from UI setSize %u %u | %p %p", width, height, fView, fFrame);

#ifdef DISTRHO_OS_MAC
        const double scaleFactor = fUI.getScaleFactor();
        width /= scaleFactor;
        height /= scaleFactor;
#endif

        v3_view_rect rect;
        std::memset(&rect, 0, sizeof(rect));
        rect.right = width;
        rect.bottom = height;
        v3_cpp_obj(fFrame)->resize_view(fFrame, fView, &rect);
    }

    static void setSizeCallback(void* ptr, uint width, uint height)
    {
        ((UIVst3*)ptr)->setSize(width, height);
    }

#if DISTRHO_PLUGIN_WANT_MIDI_INPUT
    void sendNote(const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        DISTRHO_SAFE_ASSERT_RETURN(fConnection != nullptr,);

        v3_message** const message = createMessage("midi");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        uint8_t midiData[3];
        midiData[0] = (velocity != 0 ? 0x90 : 0x80) | channel;
        midiData[1] = note;
        midiData[2] = velocity;

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(attrlist)->set_binary(attrlist, "data", midiData, sizeof(midiData));
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);
    }

    static void sendNoteCallback(void* ptr, uint8_t channel, uint8_t note, uint8_t velocity)
    {
        ((UIVst3*)ptr)->sendNote(channel, note, velocity);
    }
#endif

#if DISTRHO_PLUGIN_WANT_STATE
    void setState(const char* const key, const char* const value)
    {
        DISTRHO_SAFE_ASSERT_RETURN(fConnection != nullptr,);

        v3_message** const message = createMessage("state-set");
        DISTRHO_SAFE_ASSERT_RETURN(message != nullptr,);

        v3_attribute_list** const attrlist = v3_cpp_obj(message)->get_attributes(message);
        DISTRHO_SAFE_ASSERT_RETURN(attrlist != nullptr,);

        v3_cpp_obj(attrlist)->set_int(attrlist, "__dpf_msg_target__", 1);
        v3_cpp_obj(attrlist)->set_int(attrlist, "key:length", std::strlen(key));
        v3_cpp_obj(attrlist)->set_int(attrlist, "value:length", std::strlen(value));
        v3_cpp_obj(attrlist)->set_string(attrlist, "key", ScopedUTF16String(key));
        v3_cpp_obj(attrlist)->set_string(attrlist, "value", ScopedUTF16String(value));
        v3_cpp_obj(fConnection)->notify(fConnection, message);

        v3_cpp_obj_unref(message);
    }

    static void setStateCallback(void* ptr, const char* key, const char* value)
    {
        ((UIVst3*)ptr)->setState(key, value);
    }
#endif
};

// --------------------------------------------------------------------------------------------------------------------

/**
 * VST3 low-level pointer thingies follow, proceed with care.
 */

// --------------------------------------------------------------------------------------------------------------------
// v3_funknown for classes with a single instance

template<class T>
static uint32_t V3_API dpf_single_instance_ref(void* self)
{
    return ++(*(T**)self)->refcounter;
}

template<class T>
static uint32_t V3_API dpf_single_instance_unref(void* self)
{
    return --(*(T**)self)->refcounter;
}

// --------------------------------------------------------------------------------------------------------------------
// dpf_ui_connection_point

struct dpf_ui_connection_point : v3_connection_point_cpp {
    std::atomic_int refcounter;
    ScopedPointer<UIVst3>& uivst3;
    v3_connection_point** other;

    dpf_ui_connection_point(ScopedPointer<UIVst3>& v)
        : refcounter(1),
          uivst3(v),
          other(nullptr)
    {
        // v3_funknown, single instance
        query_interface = query_interface_connection_point;
        ref = dpf_single_instance_ref<dpf_ui_connection_point>;
        unref = dpf_single_instance_unref<dpf_ui_connection_point>;

        // v3_connection_point
        point.connect = connect;
        point.disconnect = disconnect;
        point.notify = notify;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_funknown

    static v3_result V3_API query_interface_connection_point(void* self, const v3_tuid iid, void** iface)
    {
        d_stdout("UI|query_interface_connection_point => %p", self);

        if (v3_tuid_match(iid, v3_funknown_iid))
        {
            *iface = self;
            return V3_OK;
        }

        if (v3_tuid_match(iid, v3_connection_point_iid))
        {
            *iface = self;
            return V3_OK;
        }

        *iface = NULL;
        return V3_NO_INTERFACE;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_connection_point

    static v3_result V3_API connect(void* self, v3_connection_point** other)
    {
        d_stdout("UI|dpf_ui_connection_point::connect         => %p %p", self, other);
        dpf_ui_connection_point* const point = *(dpf_ui_connection_point**)self;
        DISTRHO_SAFE_ASSERT_RETURN(point != nullptr, V3_NOT_INITIALIZED);
        DISTRHO_SAFE_ASSERT_RETURN(point->other == nullptr, V3_INVALID_ARG);

        point->other = other;

        if (UIVst3* const uivst3 = point->uivst3)
            uivst3->connect(other);

        return V3_OK;
    };

    static v3_result V3_API disconnect(void* self, v3_connection_point** other)
    {
        d_stdout("UI|dpf_ui_connection_point::disconnect      => %p %p", self, other);
        dpf_ui_connection_point* const point = *(dpf_ui_connection_point**)self;
        DISTRHO_SAFE_ASSERT_RETURN(point != nullptr, V3_NOT_INITIALIZED);
        DISTRHO_SAFE_ASSERT_RETURN(point->other != nullptr, V3_INVALID_ARG);

        point->other = nullptr;

        if (UIVst3* const uivst3 = point->uivst3)
            uivst3->disconnect();

        return V3_OK;
    };

    static v3_result V3_API notify(void* self, v3_message** message)
    {
        dpf_ui_connection_point* const point = *(dpf_ui_connection_point**)self;
        DISTRHO_SAFE_ASSERT_RETURN(point != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = point->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->notify(message);
    }
};

// --------------------------------------------------------------------------------------------------------------------
// dpf_plugin_view_content_scale

struct dpf_plugin_view_content_scale : v3_plugin_view_content_scale_cpp {
    std::atomic_int refcounter;
    ScopedPointer<UIVst3>& uivst3;
    // cached values
    float scaleFactor;

    dpf_plugin_view_content_scale(ScopedPointer<UIVst3>& v)
        : refcounter(1),
          uivst3(v),
          scaleFactor(0.0f)
    {
        // v3_funknown, single instance
        query_interface = query_interface_view_content_scale;
        ref = dpf_single_instance_ref<dpf_plugin_view_content_scale>;
        unref = dpf_single_instance_unref<dpf_plugin_view_content_scale>;

        // v3_plugin_view_content_scale
        scale.set_content_scale_factor = set_content_scale_factor;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_funknown

    static v3_result V3_API query_interface_view_content_scale(void* self, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid))
        {
            *iface = self;
            return V3_OK;
        }

        if (v3_tuid_match(iid, v3_plugin_view_content_scale_iid))
        {
            *iface = self;
            return V3_OK;
        }

        *iface = NULL;
        return V3_NO_INTERFACE;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_plugin_view_content_scale

    static v3_result V3_API set_content_scale_factor(void* self, float factor)
    {
        d_stdout("dpf_plugin_view::set_content_scale_factor => %p %f", self, factor);
        dpf_plugin_view_content_scale* const scale = *(dpf_plugin_view_content_scale**)self;
        DISTRHO_SAFE_ASSERT_RETURN(scale != nullptr, V3_NOT_INITIALIZED);

        scale->scaleFactor = factor;

        if (UIVst3* const uivst3 = scale->uivst3)
            return uivst3->setContentScaleFactor(factor);

        return V3_NOT_INITIALIZED;
    }
};

#ifdef DPF_VST3_USING_HOST_RUN_LOOP
// --------------------------------------------------------------------------------------------------------------------
// dpf_timer_handler

struct dpf_timer_handler : v3_timer_handler_cpp {
    std::atomic_int refcounter;
    ScopedPointer<UIVst3>& uivst3;
    bool valid;

    dpf_timer_handler(ScopedPointer<UIVst3>& v)
        : refcounter(1),
          uivst3(v),
          valid(true)
    {
        // v3_funknown, single instance
        query_interface = query_interface_timer_handler;
        ref = dpf_single_instance_ref<dpf_timer_handler>;
        unref = dpf_single_instance_unref<dpf_timer_handler>;

        // v3_timer_handler
        handler.on_timer = on_timer;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_funknown

    static v3_result V3_API query_interface_timer_handler(void* self, const v3_tuid iid, void** iface)
    {
        if (v3_tuid_match(iid, v3_funknown_iid))
        {
            *iface = self;
            return V3_OK;
        }

        if (v3_tuid_match(iid, v3_timer_handler_iid))
        {
            *iface = self;
            return V3_OK;
        }

        *iface = NULL;
        return V3_NO_INTERFACE;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_timer_handler

    static void V3_API on_timer(void* self)
    {
        dpf_timer_handler* const handler = *(dpf_timer_handler**)self;
        DISTRHO_SAFE_ASSERT_RETURN(handler != nullptr,);
        DISTRHO_SAFE_ASSERT_RETURN(handler->valid,);

        handler->uivst3->onTimer();
    }
};
#endif

// --------------------------------------------------------------------------------------------------------------------
// dpf_plugin_view

static const char* const kSupportedPlatforms[] = {
#ifdef _WIN32
    V3_VIEW_PLATFORM_TYPE_HWND,
#elif defined(__APPLE__)
    V3_VIEW_PLATFORM_TYPE_NSVIEW,
#else
    V3_VIEW_PLATFORM_TYPE_X11,
#endif
};

struct dpf_plugin_view : v3_plugin_view_cpp {
    std::atomic_int refcounter;
    ScopedPointer<dpf_ui_connection_point> connection;
    ScopedPointer<dpf_plugin_view_content_scale> scale;
#ifdef DPF_VST3_USING_HOST_RUN_LOOP
    ScopedPointer<dpf_timer_handler> timer;
#endif
    ScopedPointer<UIVst3> uivst3;
    // cached values
    v3_host_application** const host;
    void* const instancePointer;
    double sampleRate;
    v3_plugin_frame** frame;

    dpf_plugin_view(v3_host_application** const h, void* const instance, const double sr)
        : refcounter(1),
          host(h),
          instancePointer(instance),
          sampleRate(sr),
          frame(nullptr)
    {
        // v3_funknown, everything custom
        query_interface = query_interface_view;
        ref = ref_view;
        unref = unref_view;

        // v3_plugin_view
        view.is_platform_type_supported = is_platform_type_supported;
        view.attached = attached;
        view.removed = removed;
        view.on_wheel = on_wheel;
        view.on_key_down = on_key_down;
        view.on_key_up = on_key_up;
        view.get_size = get_size;
        view.on_size = on_size;
        view.on_focus = on_focus;
        view.set_frame = set_frame;
        view.can_resize = can_resize;
        view.check_size_constraint = check_size_constraint;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_funknown

    static v3_result V3_API query_interface_view(void* self, const v3_tuid iid, void** iface)
    {
        d_stdout("dpf_plugin_view::query_interface         => %p %s %p", self, tuid2str(iid), iface);
        *iface = NULL;
        DISTRHO_SAFE_ASSERT_RETURN(self != nullptr, V3_NO_INTERFACE);

        if (v3_tuid_match(iid, v3_funknown_iid))
        {
            *iface = self;
            return V3_OK;
        }

        if (v3_tuid_match(iid, v3_plugin_view_iid))
        {
            *iface = self;
            return V3_OK;
        }

        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NO_INTERFACE);

        if (v3_tuid_match(v3_connection_point_iid, iid))
        {
            if (view->connection == nullptr)
                view->connection = new dpf_ui_connection_point(view->uivst3);
            else
                ++view->connection->refcounter;
            *iface = &view->connection;
            return V3_OK;
        }

        if (v3_tuid_match(v3_plugin_view_content_scale_iid, iid))
        {
            if (view->scale == nullptr)
                view->scale = new dpf_plugin_view_content_scale(view->uivst3);
            else
                ++view->scale->refcounter;
            *iface = &view->scale;
            return V3_OK;
        }

        return V3_NO_INTERFACE;
    }

    static uint32_t V3_API ref_view(void* self)
    {
        return ++(*(dpf_plugin_view**)self)->refcounter;
    }

    static uint32_t V3_API unref_view(void* self)
    {
        dpf_plugin_view** const viewptr = (dpf_plugin_view**)self;
        dpf_plugin_view* const view = *viewptr;

        if (const int refcount = --view->refcounter)
        {
            d_stdout("dpf_plugin_view::unref                   => %p | refcount %i", self, refcount);
            return refcount;
        }

        d_stdout("dpf_plugin_view::unref                   => %p | refcount is zero, deleting everything now!", self);

        if (view->connection != nullptr && view->connection->other)
            v3_cpp_obj(view->connection->other)->disconnect(view->connection->other,
                                                            (v3_connection_point**)&view->connection);

        if (dpf_ui_connection_point* const conn = view->connection)
        {
            if (const int refcount = conn->refcounter)
            {
                d_stderr("DPF warning: asked to delete view while connection point still active (refcount %d)", refcount);
                return V3_INVALID_ARG;
            }
        }

        if (dpf_plugin_view_content_scale* const scale = view->scale)
        {
            if (const int refcount = scale->refcounter)
            {
                d_stderr("DPF warning: asked to delete view while content scale still active (refcount %d)", refcount);
                return V3_INVALID_ARG;
            }
        }

        delete view;
        delete viewptr;
        return 0;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // v3_plugin_view

    static v3_result V3_API is_platform_type_supported(void* self, const char* platform_type)
    {
        d_stdout("dpf_plugin_view::is_platform_type_supported => %p %s", self, platform_type);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        for (size_t i=0; i<ARRAY_SIZE(kSupportedPlatforms); ++i)
        {
            if (std::strcmp(kSupportedPlatforms[i], platform_type) == 0)
                return V3_OK;
        }

        return V3_NOT_IMPLEMENTED;
    }

    static v3_result V3_API attached(void* self, void* parent, const char* platform_type)
    {
        d_stdout("dpf_plugin_view::attached                   => %p %p %s", self, parent, platform_type);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);
        DISTRHO_SAFE_ASSERT_RETURN(view->uivst3 == nullptr, V3_INVALID_ARG);

        for (size_t i=0; i<ARRAY_SIZE(kSupportedPlatforms); ++i)
        {
            if (std::strcmp(kSupportedPlatforms[i], platform_type) == 0)
            {
               #ifdef DPF_VST3_USING_HOST_RUN_LOOP
                // find host run loop to plug ourselves into (required on some systems)
                DISTRHO_SAFE_ASSERT_RETURN(view->frame != nullptr, V3_INVALID_ARG);

                v3_run_loop** runloop = nullptr;
                v3_cpp_obj_query_interface(view->frame, v3_run_loop_iid, &runloop);
                DISTRHO_SAFE_ASSERT_RETURN(runloop != nullptr, V3_INVALID_ARG);
               #endif

                const float scaleFactor = view->scale != nullptr ? view->scale->scaleFactor : 0.0f;
                view->uivst3 = new UIVst3((v3_plugin_view**)self,
                                          view->host,
                                          view->connection != nullptr ? view->connection->other : nullptr,
                                          view->frame,
                                          (uintptr_t)parent,
                                          scaleFactor,
                                          view->sampleRate,
                                          view->instancePointer);

                view->uivst3->reconnectIfNeeded();

               #ifdef DPF_VST3_USING_HOST_RUN_LOOP
                // register a timer host run loop stuff
                view->timer = new dpf_timer_handler(view->uivst3);
                v3_cpp_obj(runloop)->register_timer(runloop,
                                                    (v3_timer_handler**)&view->timer,
                                                    DPF_VST3_TIMER_INTERVAL);
               #endif

                return V3_OK;
            }
        }

        return V3_NOT_IMPLEMENTED;
    }

    static v3_result V3_API removed(void* self)
    {
        d_stdout("dpf_plugin_view::removed                    => %p", self);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);
        DISTRHO_SAFE_ASSERT_RETURN(view->uivst3 != nullptr, V3_INVALID_ARG);

       #ifdef DPF_VST3_USING_HOST_RUN_LOOP
        // unregister our timer as needed
        if (view->timer != nullptr)
        {
            v3_run_loop** runloop = nullptr;

            if (view->frame != nullptr)
                v3_cpp_obj_query_interface(view->frame, v3_run_loop_iid, &runloop);

            if (runloop != nullptr)
            {
                v3_cpp_obj(runloop)->unregister_timer(runloop, (v3_timer_handler**)&view->timer);

                // we query it 2 times in total, so lets unref 2 times as well
                v3_cpp_obj_unref(runloop);
                v3_cpp_obj_unref(runloop);

                if (const int refcount = --view->timer->refcounter)
                {
                    view->timer->valid = false;
                    d_stderr("VST3 warning: Host run loop did not give away timer (refcount %d)", refcount);
                }
                else
                {
                    view->timer = nullptr;
                }
            }
            else
            {
                view->timer->valid = false;
                d_stderr("VST3 warning: Host run loop not available during dpf_plugin_view::removed");
            }
        }
       #endif

        view->uivst3 = nullptr;
        return V3_OK;
    }

    static v3_result V3_API on_wheel(void* self, float distance)
    {
        d_stdout("dpf_plugin_view::on_wheel                   => %p %f", self, distance);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = view->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->onWheel(distance);
    }

    static v3_result V3_API on_key_down(void* self, int16_t key_char, int16_t key_code, int16_t modifiers)
    {
        d_stdout("dpf_plugin_view::on_key_down                => %p %i %i %i", self, key_char, key_code, modifiers);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = view->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->onKeyDown(key_char, key_code, modifiers);
    }

    static v3_result V3_API on_key_up(void* self, int16_t key_char, int16_t key_code, int16_t modifiers)
    {
        d_stdout("dpf_plugin_view::on_key_up                  => %p %i %i %i", self, key_char, key_code, modifiers);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = view->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->onKeyUp(key_char, key_code, modifiers);
    }

    static v3_result V3_API get_size(void* self, v3_view_rect* rect)
    {
        d_stdout("dpf_plugin_view::get_size                   => %p", self);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        if (UIVst3* const uivst3 = view->uivst3)
            return uivst3->getSize(rect);

        // special case: allow UI to not be attached yet, as a way to get size before window creation

        const float scaleFactor = view->scale != nullptr ? view->scale->scaleFactor : 0.0f;
        UIExporter tmpUI(nullptr, 0, view->sampleRate,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         view->instancePointer, scaleFactor);
        rect->right = tmpUI.getWidth();
        rect->bottom = tmpUI.getHeight();
        return V3_OK;
    }

    static v3_result V3_API on_size(void* self, v3_view_rect* rect)
    {
        d_stdout("dpf_plugin_view::on_size                    => %p %p", self, rect);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = view->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->onSize(rect);
    }

    static v3_result V3_API on_focus(void* self, v3_bool state)
    {
        d_stdout("dpf_plugin_view::on_focus                   => %p %u", self, state);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = view->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->onFocus(state);
    }

    static v3_result V3_API set_frame(void* self, v3_plugin_frame** frame)
    {
        d_stdout("dpf_plugin_view::set_frame                  => %p %p", self, frame);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        view->frame = frame;

        if (UIVst3* const uivst3 = view->uivst3)
            return uivst3->setFrame(frame);

        return V3_NOT_INITIALIZED;
    }

    static v3_result V3_API can_resize(void* self)
    {
        d_stdout("dpf_plugin_view::can_resize                 => %p", self);
// #if DISTRHO_UI_USER_RESIZABLE
//             return V3_OK;
// #else
        return V3_NOT_IMPLEMENTED;
// #endif
    }

    static v3_result V3_API check_size_constraint(void* self, v3_view_rect* rect)
    {
        d_stdout("dpf_plugin_view::check_size_constraint      => %p %p", self, rect);
        dpf_plugin_view* const view = *(dpf_plugin_view**)self;
        DISTRHO_SAFE_ASSERT_RETURN(view != nullptr, V3_NOT_INITIALIZED);

        UIVst3* const uivst3 = view->uivst3;
        DISTRHO_SAFE_ASSERT_RETURN(uivst3 != nullptr, V3_NOT_INITIALIZED);

        return uivst3->checkSizeConstraint(rect);
    }
};

// --------------------------------------------------------------------------------------------------------------------
// dpf_plugin_view_create (called from plugin side)

v3_plugin_view** dpf_plugin_view_create(v3_host_application** host, void* instancePointer, double sampleRate);

v3_plugin_view** dpf_plugin_view_create(v3_host_application** const host,
                                        void* const instancePointer,
                                        const double sampleRate)
{
    dpf_plugin_view** const viewptr = new dpf_plugin_view*;
    *viewptr = new dpf_plugin_view(host, instancePointer, sampleRate);
    return (v3_plugin_view**)static_cast<void*>(viewptr);
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DISTRHO