#include <stdio.h>
#include <iostream>
#include <sstream>

#include <include/base/cef_bind.h>
#include <include/base/cef_callback_helpers.h>
#include <include/wrapper/cef_closure_task.h>

#include "gstcefsrc.h"
#include "gstcefaudiometa.h"

GST_DEBUG_CATEGORY_STATIC (cef_src_debug);
#define GST_CAT_DEFAULT cef_src_debug

GST_DEBUG_CATEGORY_STATIC (cef_console_debug);

#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_FPS_N 30
#define DEFAULT_FPS_D 1
#define DEFAULT_URL "https://www.google.com"
#define DEFAULT_GPU FALSE
#define DEFAULT_CHROMIUM_DEBUG_PORT -1
#define DEFAULT_LOG_SEVERITY LOGSEVERITY_DISABLE
#define DEFAULT_SANDBOX FALSE

static gboolean cef_inited = FALSE;
static gboolean init_result = FALSE;
static GMutex init_lock;
static GCond init_cond;

#define GST_TYPE_CEF_LOG_SEVERITY_MODE \
  (gst_cef_log_severity_mode_get_type ())


static GType
gst_cef_log_severity_mode_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {LOGSEVERITY_DEBUG, "debug / verbose cef log severity", "debug"},
    {LOGSEVERITY_INFO, "info cef log severity", "info"},
    {LOGSEVERITY_WARNING, "warning cef log severity", "warning"},
    {LOGSEVERITY_ERROR, "error cef log severity", "error"},
    {LOGSEVERITY_FATAL, "fatal cef log severity", "fatal"},
    {LOGSEVERITY_DISABLE, "disable cef log severity", "disable"},
    {0, NULL, NULL},
  };

  if (!type) {
    type = g_enum_register_static ("GstCefLogSeverityMode", values);
  }
  return type;
}

enum
{
  PROP_0,
  PROP_URL,
  PROP_GPU,
  PROP_CHROMIUM_DEBUG_PORT,
  PROP_CHROME_EXTRA_FLAGS,
  PROP_SANDBOX,
  PROP_JS_FLAGS,
  PROP_LOG_SEVERITY,
  PROP_CEF_CACHE_LOCATION,
};

#define gst_cef_src_parent_class parent_class
G_DEFINE_TYPE (GstCefSrc, gst_cef_src, GST_TYPE_PUSH_SRC);

#define CEF_VIDEO_CAPS "video/x-raw, format=BGRA, width=[1, 2147483647], height=[1, 2147483647], framerate=[1/1, 60/1], pixel-aspect-ratio=1/1"
#define CEF_AUDIO_CAPS "audio/x-raw, format=F32LE, rate=[1, 2147483647], channels=[1, 2147483647], layout=interleaved"

static GstStaticPadTemplate gst_cef_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CEF_VIDEO_CAPS)
    );

gchar* get_plugin_base_path () {
  GstPlugin *plugin = gst_registry_find_plugin(gst_registry_get(), "cef");
  gchar* base_path = g_path_get_dirname(gst_plugin_get_filename(plugin));
  gst_object_unref(plugin);
  return base_path;
}

static void gst_cef_src_log_add(GstCefSrc *src, const char *format, ...) {
  va_list args;
  va_start(args, format);
  char log_buf[256];
  vsnprintf(log_buf, sizeof (log_buf) - 1, format, args);
  log_buf[sizeof (log_buf) - 1] = 0;
  va_end(args);

  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  GstClockTime time = GST_TIMESPEC_TO_TIME(ts);

  GST_OBJECT_LOCK (src);
  if (src->log_queue) {
    src->log_queue->push(GstCefLogItem(time, log_buf));
    while (src->log_queue->size() > 50)
      src->log_queue->pop();
  }
  GST_OBJECT_UNLOCK (src);
}

static void gst_cef_src_log_flush(GstCefSrc *src) {
  GST_OBJECT_LOCK (src);

  if (src->log_queue) {
    int index = 0;
    while (src->log_queue->size()) {
      GstCefLogItem log = src->log_queue->front();
      src->log_queue->pop();

      GST_WARNING_OBJECT(src, "Pre #%i: %" GST_TIME_FORMAT " %s", index, GST_TIME_ARGS(log.time), log.log.c_str());
      index++;
    }
  }

  GST_OBJECT_UNLOCK (src);
}

static gboolean gst_cef_src_check_time(GstCefSrc *src, gboolean audio_flag) {
  GstClock *clock = GST_ELEMENT_CLOCK (src);
  if (!clock) {
    GST_WARNING_OBJECT(src, "Can't get element clock object");
    return FALSE;
  }

  gst_object_ref (clock);
  GstClockTime now = gst_clock_get_time (clock);
  gst_object_unref (clock);

  GstClockTime base_time = GST_ELEMENT_CAST (src)->base_time;
  GstClockTime pipeline_time = now - base_time;

  guint64 video_frame_index = src->video_frame_index;
  GstClockTime video_frame_time = gst_util_uint64_scale (src->video_frame_index, src->vinfo.fps_d * GST_SECOND, src->vinfo.fps_n);
  GstClockTime video_frame_duration = gst_util_uint64_scale (GST_SECOND, src->vinfo.fps_d, src->vinfo.fps_n);

  GstClockTime audio_frame_time = src->audio_frame_time;
//  GstClockTime audio_frame_duration = src->audio_frame_duration;

  if ((pipeline_time >= video_frame_duration && video_frame_time < pipeline_time - video_frame_duration)
      || video_frame_time > pipeline_time + video_frame_duration * 5) {

    guint64 new_index = gst_util_uint64_scale (pipeline_time, src->vinfo.fps_n, src->vinfo.fps_d * GST_SECOND);

    // If we have neither video nor audio frames from CEF, we allow the pipeline to have lags and
    // jump forwards without warnings
    if (audio_flag || new_index < video_frame_index || new_index > video_frame_index + 10) {
      GST_WARNING_OBJECT(
        src,
        "Correct video time"
        ", cur index=%" G_GUINT64_FORMAT
          ", cur time=%" GST_TIME_FORMAT
          ", new index=%" G_GUINT64_FORMAT
          ", duration=%" GST_TIME_FORMAT
          ", pipeline=%" GST_TIME_FORMAT,
        video_frame_index,
        GST_TIME_ARGS(video_frame_time),
        new_index,
        GST_TIME_ARGS(video_frame_duration),
        GST_TIME_ARGS(pipeline_time)
      );

      gst_cef_src_log_flush(src);
    }

    video_frame_index = new_index;
    video_frame_time = gst_util_uint64_scale (src->video_frame_index, src->vinfo.fps_d * GST_SECOND, src->vinfo.fps_n);
  }

  if (audio_frame_time < video_frame_time) {
    GstClockTime new_time = video_frame_time + video_frame_duration;

    if (audio_flag) {
      GST_WARNING_OBJECT(
        src,
        "Correct audio time"
          ", cur time=%" GST_TIME_FORMAT
          ", new time=%" GST_TIME_FORMAT
//          ", duration=%" GST_TIME_FORMAT
          ", video time=%" GST_TIME_FORMAT
          ", video duration=%" GST_TIME_FORMAT
          ", pipeline=%" GST_TIME_FORMAT,
        GST_TIME_ARGS(audio_frame_time),
        GST_TIME_ARGS(new_time),
//        GST_TIME_ARGS(audio_frame_duration),
        GST_TIME_ARGS(video_frame_time),
        GST_TIME_ARGS(video_frame_duration),
        GST_TIME_ARGS(pipeline_time)
      );

      gst_cef_src_log_flush(src);
    }

    audio_frame_time = new_time;
  }

  gst_cef_src_log_add(
    src,
    "Check time"
      ", video index=%" G_GUINT64_FORMAT
      ", video time=%" GST_TIME_FORMAT
      ", video duration=%" GST_TIME_FORMAT
      ", audio time=%" GST_TIME_FORMAT
      ", pipeline=%" GST_TIME_FORMAT,
    video_frame_index,
    GST_TIME_ARGS(video_frame_time),
    GST_TIME_ARGS(video_frame_duration),
    GST_TIME_ARGS(audio_frame_time),
    GST_TIME_ARGS(pipeline_time)
  );

  GST_OBJECT_LOCK (src);
  //src->global_frame_time = now;
  src->video_frame_index = video_frame_index;
  src->audio_frame_time = audio_frame_time;
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}


class RenderHandler : public CefRenderHandler
{
  public:

    RenderHandler(GstCefSrc *element) :
        element (element)
    {
    }

    ~RenderHandler()
    {
    }

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override
    {
      GST_OBJECT_LOCK (element);
      rect = CefRect(0, 0, element->vinfo.width ? element->vinfo.width : DEFAULT_WIDTH, element->vinfo.height ? element->vinfo.height : DEFAULT_HEIGHT);
      GST_OBJECT_UNLOCK (element);
    }

    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList &dirtyRects, const void * buffer, int w, int h) override
    {
      gst_cef_src_log_add(element, "Capture video, width=%d, height=%d", w, h);

      GstBuffer *new_buffer;
      new_buffer = gst_buffer_new_allocate (NULL, element->vinfo.width * element->vinfo.height * 4, NULL);
      gst_buffer_fill (new_buffer, 0, buffer, w * h * 4);

      GST_OBJECT_LOCK (element);
      gst_buffer_replace (&(element->current_buffer), new_buffer);
      gst_buffer_unref (new_buffer);
      GST_OBJECT_UNLOCK (element);
    }

  private:

    GstCefSrc *element;

    IMPLEMENT_REFCOUNTING(RenderHandler);
};

class RequestHandler : public CefRequestHandler
{
  public:

    RequestHandler(GstCefSrc *element) :
        element (element)
    {
    }

    ~RequestHandler()
    {
    }

    virtual void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status) override
		{
			GST_WARNING_OBJECT (element, "Render subprocess terminated, reloading URL!");
      browser->Reload();
    }

  private:

    GstCefSrc *element;
    IMPLEMENT_REFCOUNTING(RequestHandler);
};


class AudioHandler : public CefAudioHandler
{
  public:

    AudioHandler(GstCefSrc *element) :
        mElement (element)
    {
    }

    ~AudioHandler()
    {
    }

  void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                            const CefAudioParameters& params,
                            int channels) override
  {
    GstStructure *s = gst_structure_new ("cef-audio-stream-start",
        "channels", G_TYPE_INT, channels,
        "rate", G_TYPE_INT, params.sample_rate,
        NULL);
    GstEvent *event = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, s);

    mRate = params.sample_rate;
    mChannels = channels;
    //mCurrentTime = GST_CLOCK_TIME_NONE;

    GST_OBJECT_LOCK (mElement);
    mElement->audio_events = g_list_append (mElement->audio_events, event);
    GST_OBJECT_UNLOCK (mElement);
  }

  void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser,
                           const float** data,
                           int frames,
                           int64_t pts) override
  {
    gst_cef_src_log_add(mElement,
                        "Capture audio, samples=%d, time=%" GST_TIME_FORMAT,
                        frames,
                        GST_TIME_ARGS(mElement->audio_frame_time));

    GstBuffer *buf;
    GstMapInfo info;
    gint i, j;

    buf = gst_buffer_new_allocate (NULL, mChannels * frames * 4, NULL);

    gst_buffer_map (buf, &info, GST_MAP_WRITE);
    for (i = 0; i < mChannels; i++) {
      gfloat *cdata = (gfloat *) data[i];

      for (j = 0; j < frames; j++) {
        memcpy (info.data + j * 4 * mChannels + i * 4, &cdata[j], 4);
      }
    }
    gst_buffer_unmap (buf, &info);

    // Write all audio data to the file
    // TODO Remove for production
    FILE *f = fopen("audio.raw", "a");
    GstMapInfo info2;
    gst_buffer_map (buf, &info2, GST_MAP_WRITE);
    fwrite(info2.data, info2.size, 1, f);
    gst_buffer_unmap (buf, &info2);
    fclose(f);

    GstClockTime audio_frame_duration = gst_util_uint64_scale (frames, GST_SECOND, mRate);

    // First audio frame pts in buffer has to be synced with video
    if (!mElement->audio_buffers) {
      GstClockTime video_frame_time = gst_util_uint64_scale (mElement->video_frame_index,
                                                             mElement->vinfo.fps_d * GST_SECOND,
                                                             mElement->vinfo.fps_n);

      GstClockTime video_frame_duration = gst_util_uint64_scale (GST_SECOND,
                                                                 mElement->vinfo.fps_d,
                                                                 mElement->vinfo.fps_n);

      if (mElement->audio_frame_time > video_frame_time + video_frame_duration * 3) {
        GstClockTime new_time = video_frame_time + video_frame_duration;

        GST_WARNING_OBJECT(
            mElement,
            "Correct audio time"
            ", cur time=%" GST_TIME_FORMAT
                ", new time=%" GST_TIME_FORMAT
                ", duration=%" GST_TIME_FORMAT
                ", video time=%" GST_TIME_FORMAT
                ", video duration=%" GST_TIME_FORMAT,
            GST_TIME_ARGS(mElement->audio_frame_time),
            GST_TIME_ARGS(new_time),
            GST_TIME_ARGS(audio_frame_duration),
            GST_TIME_ARGS(video_frame_time),
            GST_TIME_ARGS(video_frame_duration)
        );

        gst_cef_src_log_flush(mElement);

        GST_OBJECT_LOCK (mElement);
        mElement->audio_frame_time = new_time;
        GST_OBJECT_UNLOCK (mElement);
      }
    }

    GST_OBJECT_LOCK (mElement);

    // TODO Correct pts has to be: audio_frame_time - audio_frame_duration + (now - global_frame_time)
    GST_BUFFER_PTS (buf) = mElement->audio_frame_time;
    GST_BUFFER_DTS (buf) = mElement->audio_frame_time;
    GST_BUFFER_DURATION (buf) = audio_frame_duration;

    mElement->audio_frame_time += audio_frame_duration;
//    mElement->audio_frame_duration = audio_frame_duration;

    if (!mElement->audio_buffers)
      mElement->audio_buffers = gst_buffer_list_new();

    gst_buffer_list_add (mElement->audio_buffers, buf);
    GST_OBJECT_UNLOCK (mElement);
  }

  void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override
  {
  }

  void OnAudioStreamError(CefRefPtr<CefBrowser> browser,
                          const CefString& message) override {
    GST_WARNING_OBJECT (mElement, "Audio stream error: %s", message.ToString().c_str());
  }

  private:

    GstCefSrc *mElement;
    //GstClockTime mCurrentTime;
    gint mRate;
    gint mChannels;
    IMPLEMENT_REFCOUNTING(AudioHandler);
};


class DisplayHandler : public CefDisplayHandler {
public:
  DisplayHandler(GstCefSrc *element) : mElement(element) {}

  ~DisplayHandler() = default;

  virtual bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString &message, const CefString &source, int line) override {
    GstDebugLevel gst_level = GST_LEVEL_NONE;
    switch (level) {
    case LOGSEVERITY_DEFAULT:
    case LOGSEVERITY_INFO:
      gst_level = GST_LEVEL_INFO;
      break;
    case LOGSEVERITY_DEBUG:
      gst_level = GST_LEVEL_DEBUG;
      break;
    case LOGSEVERITY_WARNING:
      gst_level = GST_LEVEL_WARNING;
      break;
    case LOGSEVERITY_ERROR:
    case LOGSEVERITY_FATAL:
      gst_level = GST_LEVEL_ERROR;
      break;
    case LOGSEVERITY_DISABLE:
      gst_level = GST_LEVEL_NONE;
      break;
    };
    GST_CAT_LEVEL_LOG (cef_console_debug, gst_level, mElement, "%s:%d %s", source.ToString().c_str(), line,
      message.ToString().c_str());
    return false;
  }

private:
  GstCefSrc *mElement;
  IMPLEMENT_REFCOUNTING(DisplayHandler);
};


class BrowserClient :
  public CefClient,
  public CefLifeSpanHandler
{
  public:

    BrowserClient(CefRefPtr<CefRenderHandler> rptr, CefRefPtr<CefAudioHandler> aptr, CefRefPtr<CefRequestHandler> rqptr, CefRefPtr<CefDisplayHandler> display_handler, GstCefSrc *element) :
        render_handler(rptr),
        audio_handler(aptr),
        request_handler(rqptr),
        display_handler(display_handler),
        mElement(element)
    {
    }

    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    virtual CefRefPtr<CefRenderHandler> GetRenderHandler() override
    {
      return render_handler;
    }

    virtual CefRefPtr<CefAudioHandler> GetAudioHandler() override
    {
      return audio_handler;
    }

    virtual CefRefPtr<CefRequestHandler> GetRequestHandler() override
    {
      return request_handler;
    }

    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override
    {
      return display_handler;
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    void MakeBrowser(int);
    void CloseBrowser(int);

  private:

    CefRefPtr<CefRenderHandler> render_handler;
    CefRefPtr<CefAudioHandler> audio_handler;
    CefRefPtr<CefRequestHandler> request_handler;
    CefRefPtr<CefDisplayHandler> display_handler;

  public:
    GstCefSrc *mElement;

    IMPLEMENT_REFCOUNTING(BrowserClient);
};

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  mElement->browser = nullptr;
  g_mutex_lock (&mElement->state_lock);
  mElement->started = FALSE;
  g_cond_signal (&mElement->state_cond);
  g_mutex_unlock(&mElement->state_lock);
}

void BrowserClient::MakeBrowser(int arg)
{
  CefWindowInfo window_info;
  CefRefPtr<CefBrowser> browser;
  CefBrowserSettings browser_settings;

  window_info.SetAsWindowless(0);
  browser = CefBrowserHost::CreateBrowserSync(window_info, this, std::string(mElement->url), browser_settings, nullptr, nullptr);

  browser->GetHost()->SetAudioMuted(true);

  mElement->browser = browser;

  g_mutex_lock (&mElement->state_lock);
  mElement->started = TRUE;
  g_cond_signal (&mElement->state_cond);
  g_mutex_unlock(&mElement->state_lock);
}

void BrowserClient::CloseBrowser(int arg)
{
  mElement->browser->GetHost()->CloseBrowser(true);
}

class App : public CefApp
{
  public:
    App(GstCefSrc *src) : src(src)
    {
    }

  virtual void OnBeforeCommandLineProcessing(const CefString &process_type,
                                             CefRefPtr<CefCommandLine> command_line) override
  {
    command_line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
    command_line->AppendSwitch("enable-media-stream");
    command_line->AppendSwitch("disable-dev-shm-usage"); /* https://github.com/GoogleChrome/puppeteer/issues/1834 */
    command_line->AppendSwitch("enable-begin-frame-scheduling"); /* https://bitbucket.org/chromiumembedded/cef/issues/1368 */

    if (!src->gpu) {
      // Optimize for no gpu usage
      command_line->AppendSwitch("disable-gpu");
      command_line->AppendSwitch("disable-gpu-compositing");
    }

    if (src->chromium_debug_port >= 0) {
      command_line->AppendSwitchWithValue("remote-debugging-port", g_strdup_printf ("%i", src->chromium_debug_port));
    }

    if (src->chrome_extra_flags) {
      gchar **flags_list = g_strsplit ((const gchar *) src->chrome_extra_flags, ",", -1);
      guint i;

      for (i = 0; i < g_strv_length (flags_list); i++) {
        gchar **switch_value = g_strsplit ((const gchar *) flags_list[i], "=", -1);

        if (g_strv_length (switch_value) > 1) {
          GST_INFO_OBJECT (src, "Adding switch with value %s=%s", switch_value[0], switch_value[1]);
          command_line->AppendSwitchWithValue (switch_value[0], switch_value[1]);
        } else {
          GST_INFO_OBJECT (src, "Adding flag %s", flags_list[i]);
          command_line->AppendSwitch (flags_list[i]);
        }

        g_strfreev (switch_value);
      }

      g_strfreev (flags_list);
    }
  }

 private:
  IMPLEMENT_REFCOUNTING(App);
  GstCefSrc *src;
};


static GstFlowReturn gst_cef_src_create(GstPushSrc *push_src, GstBuffer **buf)
{
  GstCefSrc *src = GST_CEF_SRC (push_src);
  GList *tmp;

  GstClockTime frame_time = gst_util_uint64_scale (src->video_frame_index,
                                                   src->vinfo.fps_d * GST_SECOND,
                                                   src->vinfo.fps_n);

  GstClockTime frame_duration = gst_util_uint64_scale (GST_SECOND,
                                                       src->vinfo.fps_d,
                                                       src->vinfo.fps_n);

  gst_cef_src_log_add(src,
                      "Push frame, index=%" G_GUINT64_FORMAT ", time=%" GST_TIME_FORMAT,
                      src->video_frame_index,
                      GST_TIME_ARGS(frame_time));

  GST_OBJECT_LOCK (src);

  if (src->audio_events) {
    for (tmp = src->audio_events; tmp; tmp = tmp->next) {
      gst_pad_push_event (GST_BASE_SRC_PAD (src), (GstEvent *) tmp->data);
    }

    g_list_free (src->audio_events);
    src->audio_events = NULL;
  }

  g_assert (src->current_buffer);
  *buf = gst_buffer_copy (src->current_buffer);

  gboolean audio_flag = false;
  if (src->audio_buffers) {
    gst_buffer_add_cef_audio_meta (*buf, src->audio_buffers);
    src->audio_buffers = NULL;
    audio_flag = true;
  }

  GST_BUFFER_PTS (*buf) = frame_time;
  GST_BUFFER_DTS (*buf) = frame_time;
  GST_BUFFER_DURATION (*buf) = frame_duration;
  src->video_frame_index++;

  GST_OBJECT_UNLOCK (src);

  if (!gst_cef_src_check_time(src, audio_flag))
    return GST_FLOW_ERROR;

  return GST_FLOW_OK;
}


/* Once we have started a first cefsrc for this process, we start
 * a UI thread and never shut it down. We could probably refine this
 * to stop and restart the thread as needed, but this updated approach
 * now no longer requires a main loop to be running, doesn't crash
 * when one is running either with CEF 86+, and allows for multiple
 * concurrent cefsrc instances.
 */
static gpointer
run_cef (GstCefSrc *src)
{
#ifdef G_OS_WIN32
  HINSTANCE hInstance = GetModuleHandle(NULL);
  CefMainArgs args(hInstance);
#else
  CefMainArgs args(0, NULL);
#endif

  CefSettings settings;
  CefRefPtr<App> app;
  CefWindowInfo window_info;
  CefBrowserSettings browserSettings;

  settings.no_sandbox = !src->sandbox;
  settings.windowless_rendering_enabled = true;
  settings.log_severity = src->log_severity;

  GST_INFO  ("Initializing CEF");

  gchar* base_path = get_plugin_base_path();

  // If not absolute path append to current_dir
  if (!g_path_is_absolute(base_path)) {
    gchar* current_dir = g_get_current_dir();

    gchar* old_base_path = base_path;
    base_path = g_build_filename(current_dir, base_path, NULL);

    g_free(current_dir);
    g_free(old_base_path);
  }

  gchar* browser_subprocess_path = g_build_filename(base_path, "gstcefsubprocess", NULL);
  if (const gchar *custom_subprocess_path = g_getenv ("GST_CEF_SUBPROCESS_PATH")) {
    g_setenv ("CEF_SUBPROCESS_PATH", browser_subprocess_path, TRUE);
    g_free (browser_subprocess_path);
    browser_subprocess_path = g_strdup (custom_subprocess_path);
  }

  CefString(&settings.browser_subprocess_path).FromASCII(browser_subprocess_path);
  g_free(browser_subprocess_path);

  gchar *locales_dir_path = g_build_filename(base_path, "locales", NULL);
  CefString(&settings.locales_dir_path).FromASCII(locales_dir_path);

  if (src->js_flags != NULL) {
    CefString(&settings.javascript_flags).FromASCII(src->js_flags);
  }

  if (src->cef_cache_location != NULL) {
    CefString(&settings.cache_path).FromASCII(src->cef_cache_location);
  }

  g_free(base_path);
  g_free(locales_dir_path);

  app = new App(src);

  if (!CefInitialize(args, settings, app, nullptr)) {
    GST_ERROR ("Failed to initialize CEF");

    /* unblock start () */
    g_mutex_lock (&init_lock);
    cef_inited = TRUE;
    g_cond_signal(&init_cond);
    g_mutex_unlock (&init_lock);

    goto done;
  }

  g_mutex_lock (&init_lock);
  cef_inited = TRUE;
  init_result = TRUE;
  g_cond_signal(&init_cond);
  g_mutex_unlock (&init_lock);

  CefRunMessageLoop();

  CefShutdown();

  g_mutex_lock (&init_lock);
  cef_inited = FALSE;
  g_cond_signal(&init_cond);
  g_mutex_unlock (&init_lock);

done:
  return NULL;
}

void quit_message_loop (int arg)
{
  CefQuitMessageLoop();
}

class ShutdownEnforcer {
 public:
  ~ShutdownEnforcer() {
    if (!cef_inited)
      return;

    CefPostTask(TID_UI, base::BindOnce(&quit_message_loop, 0));

    g_mutex_lock(&init_lock);
    while (cef_inited)
      g_cond_wait (&init_cond, &init_lock);
    g_mutex_unlock (&init_lock);
  }
} shutdown_enforcer;

static gpointer
init_cef (gpointer src)
{
  g_mutex_init (&init_lock);
  g_cond_init (&init_cond);

  g_thread_new("cef-ui-thread", (GThreadFunc) run_cef, src);

  return NULL;
}

static gboolean
gst_cef_src_start(GstBaseSrc *base_src)
{
  static GOnce init_once = G_ONCE_INIT;
  gboolean ret = FALSE;

  GstCefSrc *src = GST_CEF_SRC (base_src);
  CefRefPtr<BrowserClient> browserClient;
  CefRefPtr<RenderHandler> renderHandler = new RenderHandler(src);
  CefRefPtr<AudioHandler> audioHandler = new AudioHandler(src);
  CefRefPtr<RequestHandler> requestHandler = new RequestHandler(src);
  CefRefPtr<DisplayHandler> displayHandler = new DisplayHandler(src);

  /* Initialize global variables */
  g_once (&init_once, init_cef, src);

  /* Make sure CEF is initialized before posting a task */
  g_mutex_lock (&init_lock);
  while (!cef_inited)
    g_cond_wait (&init_cond, &init_lock);
  g_mutex_unlock (&init_lock);

  if (!init_result)
    goto done;

  GST_OBJECT_LOCK (src);
//  src->global_frame_time = 0;
  src->video_frame_index = 0;
  src->audio_frame_time = 0;
  src->log_queue = new std::queue<GstCefLogItem>();
//  src->audio_frame_duration = 0;
  GST_OBJECT_UNLOCK (src);

  browserClient = new BrowserClient(renderHandler, audioHandler, requestHandler, displayHandler, src);
  CefPostTask(TID_UI, base::BindOnce(&BrowserClient::MakeBrowser, browserClient.get(), 0));

  /* And wait for this src's browser to have been created */
  g_mutex_lock(&src->state_lock);
  while (!src->started)
    g_cond_wait (&src->state_cond, &src->state_lock);
  g_mutex_unlock (&src->state_lock);

  ret = src->browser != NULL;

done:
  return ret;
}

static gboolean
gst_cef_src_stop (GstBaseSrc *base_src)
{
  GstCefSrc *src = GST_CEF_SRC (base_src);

  GST_INFO_OBJECT (src, "Stopping");

  if (src->browser) {
    src->browser->GetHost()->CloseBrowser(true);

    /* And wait for this src's browser to have been closed */
    g_mutex_lock(&src->state_lock);
    while (src->started)
      g_cond_wait (&src->state_cond, &src->state_lock);
    g_mutex_unlock (&src->state_lock);
  }

  GST_OBJECT_LOCK (src);
  delete src->log_queue;
  src->log_queue = NULL;
  GST_OBJECT_UNLOCK(src);

  return TRUE;
}

static void
gst_cef_src_get_times (GstBaseSrc * base_src, GstBuffer * buffer, GstClockTime * start, GstClockTime * end)
{
  GstClockTime timestamp = GST_BUFFER_PTS (buffer);
  GstClockTime duration = GST_BUFFER_DURATION (buffer);

  *start = timestamp;
  *end = timestamp + duration;

  // GST_LOG_OBJECT (base_src, "Got times start: %" GST_TIME_FORMAT " end: %" GST_TIME_FORMAT, GST_TIME_ARGS (*start), GST_TIME_ARGS (*end));
}


static gboolean
gst_cef_src_query (GstBaseSrc * base_src, GstQuery * query)
{
  gboolean res = FALSE;
  GstCefSrc *src = GST_CEF_SRC (base_src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      GstClockTime latency;

      if (src->vinfo.fps_n) {
        latency = gst_util_uint64_scale (GST_SECOND, src->vinfo.fps_d, src->vinfo.fps_n);
        GST_DEBUG_OBJECT (src, "Reporting latency: %" GST_TIME_FORMAT, GST_TIME_ARGS (latency));
        gst_query_set_latency (query, TRUE, latency, GST_CLOCK_TIME_NONE);
      }
      res = TRUE;
      break;
    }
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (base_src, query);
      break;
  }

  return res;
}

static GstCaps *
gst_cef_src_fixate (GstBaseSrc * base_src, GstCaps * caps)
{
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);
  structure = gst_caps_get_structure (caps, 0);

  gst_structure_fixate_field_nearest_int (structure, "width", DEFAULT_WIDTH);
  gst_structure_fixate_field_nearest_int (structure, "height", DEFAULT_HEIGHT);

  if (gst_structure_has_field (structure, "framerate"))
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", DEFAULT_FPS_N, DEFAULT_FPS_D);
  else
    gst_structure_set (structure, "framerate", GST_TYPE_FRACTION, DEFAULT_FPS_N, DEFAULT_FPS_D, NULL);

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (base_src, caps);

  GST_INFO_OBJECT (base_src, "Fixated caps to %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
gst_cef_src_set_caps (GstBaseSrc * base_src, GstCaps * caps)
{
  GstCefSrc *src = GST_CEF_SRC (base_src);
  GstBuffer *new_buffer;

  GST_INFO_OBJECT (base_src, "Caps set to %" GST_PTR_FORMAT, caps);

  GST_OBJECT_LOCK (src);
  gst_video_info_from_caps (&src->vinfo, caps);
  new_buffer = gst_buffer_new_allocate (NULL, src->vinfo.width * src->vinfo.height * 4, NULL);
  gst_buffer_replace (&(src->current_buffer), new_buffer);
  gst_buffer_unref (new_buffer);
  src->browser->GetHost()->SetWindowlessFrameRate(gst_util_uint64_scale (1, src->vinfo.fps_n, src->vinfo.fps_d));
  src->browser->GetHost()->WasResized();
  GST_OBJECT_UNLOCK (src);

  //gst_cef_src_check_n_frames(src);
  return TRUE;
}


static void
gst_cef_src_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstCefSrc *src = GST_CEF_SRC (object);

  switch (prop_id) {
    case PROP_URL:
    {
      const gchar *url;

      url = g_value_get_string (value);
      g_free (src->url);
      src->url = g_strdup (url);

      g_mutex_lock(&src->state_lock);
      if (src->started) {
        src->browser->GetMainFrame()->LoadURL(src->url);
      }
      g_mutex_unlock(&src->state_lock);

      break;
    }
    case PROP_CHROME_EXTRA_FLAGS: {
      g_free (src->chrome_extra_flags);
      src->chrome_extra_flags = g_value_dup_string (value);
      break;
    }
    case PROP_GPU:
    {
      src->gpu = g_value_get_boolean (value);
      break;
    }
    case PROP_CHROMIUM_DEBUG_PORT:
    {
      src->chromium_debug_port = g_value_get_int (value);
      break;
    }
    case PROP_SANDBOX:
    {
      src->sandbox = g_value_get_boolean (value);
      break;
    }
    case PROP_JS_FLAGS: {
      g_free (src->js_flags);
      src->js_flags = g_value_dup_string (value);
      break;
    }
    case PROP_LOG_SEVERITY: {
      src->log_severity = (cef_log_severity_t) g_value_get_enum (value);
      break;
    }
    case PROP_CEF_CACHE_LOCATION: {
      g_free (src->cef_cache_location);
      src->cef_cache_location = g_value_dup_string (value);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cef_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstCefSrc *src = GST_CEF_SRC (object);

  switch (prop_id) {
    case PROP_URL:
      g_value_set_string (value, src->url);
      break;
    case PROP_CHROME_EXTRA_FLAGS:
      g_value_set_string (value, src->chrome_extra_flags);
      break;
    case PROP_GPU:
      g_value_set_boolean (value, src->gpu);
      break;
    case PROP_CHROMIUM_DEBUG_PORT:
      g_value_set_int (value, src->chromium_debug_port);
      break;
    case PROP_SANDBOX:
      g_value_set_boolean (value, src->sandbox);
      break;
    case PROP_JS_FLAGS:
      g_value_set_string (value, src->js_flags);
      break;
    case PROP_LOG_SEVERITY:
      g_value_set_enum (value, src->log_severity);
      break;
    case PROP_CEF_CACHE_LOCATION:
      g_value_set_string (value, src->cef_cache_location);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cef_src_finalize (GObject *object)
{
  GstCefSrc *src = GST_CEF_SRC (object);

  if (src->audio_buffers) {
    gst_buffer_list_unref (src->audio_buffers);
    src->audio_buffers = NULL;
  }

  g_list_free_full (src->audio_events, (GDestroyNotify) gst_event_unref);
  src->audio_events = NULL;

  g_free (src->js_flags);
  g_free (src->cef_cache_location);

  g_cond_clear(&src->state_cond);
  g_mutex_clear(&src->state_lock);
}

static void
gst_cef_src_init (GstCefSrc * src)
{
  GstBaseSrc *base_src = GST_BASE_SRC (src);

//  src->global_frame_time = 0;
  src->video_frame_index = 0;
  src->audio_frame_time = 0;
//  src->audio_frame_duration = 0;

  src->current_buffer = NULL;
  src->audio_buffers = NULL;
  src->audio_events = NULL;
  src->started = FALSE;
  src->chromium_debug_port = DEFAULT_CHROMIUM_DEBUG_PORT;
  src->sandbox = DEFAULT_SANDBOX;
  src->js_flags = NULL;
  src->log_severity = DEFAULT_LOG_SEVERITY;
  src->cef_cache_location = NULL;

  gst_base_src_set_format (base_src, GST_FORMAT_TIME);
  gst_base_src_set_live (base_src, TRUE);
  gst_base_src_set_do_timestamp (base_src, FALSE);

  g_cond_init (&src->state_cond);
  g_mutex_init (&src->state_lock);
}

static void
gst_cef_src_class_init (GstCefSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
  GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);

  gobject_class->set_property = gst_cef_src_set_property;
  gobject_class->get_property = gst_cef_src_get_property;
  gobject_class->finalize = gst_cef_src_finalize;

  g_object_class_install_property (gobject_class, PROP_URL,
      g_param_spec_string ("url", "url",
          "The URL to display",
          DEFAULT_URL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT)));

  g_object_class_install_property (gobject_class, PROP_GPU,
    g_param_spec_boolean ("gpu", "gpu",
          "Enable GPU usage in chromium (Improves performance if you have GPU)",
          DEFAULT_GPU, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CHROMIUM_DEBUG_PORT,
    g_param_spec_int ("chromium-debug-port", "chromium-debug-port",
          "Set chromium debug port (-1 = disabled) "
          "deprecated: use chrome-extra-flags instead", -1, G_MAXUINT16,
          DEFAULT_CHROMIUM_DEBUG_PORT, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CHROME_EXTRA_FLAGS,
    g_param_spec_string ("chrome-extra-flags", "chrome-extra-flags",
          "Comma delimiter flags to be passed into chrome "
          "(Example: show-fps-counter,remote-debugging-port=9222)",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_SANDBOX,
    g_param_spec_boolean ("sandbox", "sandbox",
          "Toggle chromium sandboxing capabilities",
          DEFAULT_SANDBOX, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_JS_FLAGS,
    g_param_spec_string ("js-flags", "js-flags",
          "Space delimited JavaScript flags to be passed to Chromium "
          "(Example: --noexpose_wasm --expose-gc)",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_LOG_SEVERITY,
      g_param_spec_enum ("log-severity", "log-severity",
          "CEF log severity level",
          GST_TYPE_CEF_LOG_SEVERITY_MODE, DEFAULT_LOG_SEVERITY,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (gobject_class, PROP_CEF_CACHE_LOCATION,
    g_param_spec_string ("cef-cache-location", "cef-cache-location",
          "Cache location for CEF. Defaults to in memory cache. "
          "(Example: /tmp/cef-cache/)",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  gst_element_class_set_static_metadata (gstelement_class,
      "Chromium Embedded Framework source", "Source/Video",
      "Creates a video stream from an embedded Chromium browser", "Mathieu Duponchelle <mathieu@centricular.com>");

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_cef_src_template);

  base_src_class->fixate = GST_DEBUG_FUNCPTR(gst_cef_src_fixate);
  base_src_class->set_caps = GST_DEBUG_FUNCPTR(gst_cef_src_set_caps);
  base_src_class->start = GST_DEBUG_FUNCPTR(gst_cef_src_start);
  base_src_class->stop = GST_DEBUG_FUNCPTR(gst_cef_src_stop);
  base_src_class->get_times = GST_DEBUG_FUNCPTR(gst_cef_src_get_times);
  base_src_class->query = GST_DEBUG_FUNCPTR(gst_cef_src_query);

  push_src_class->create = GST_DEBUG_FUNCPTR(gst_cef_src_create);

  GST_DEBUG_CATEGORY_INIT (cef_src_debug, "cefsrc", 0,
      "Chromium Embedded Framework Source");
  GST_DEBUG_CATEGORY_INIT (cef_console_debug, "cefconsole", 0,
      "Chromium Embedded Framework JS Console");
}
