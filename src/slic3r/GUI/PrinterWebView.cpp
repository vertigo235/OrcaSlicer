#include "PrinterWebView.hpp"

#include "PrinterWebViewHandler.hpp"
#include "I18N.hpp"

#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>

#include "libslic3r_version.h"
#include <libslic3r/PrintConfig.hpp>
#include "libslic3r/Utils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem/path.hpp>

#include <nlohmann/json.hpp>

#include <wx/chartype.h>
#include <wx/event.h>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/toolbar.h>
#include "slic3r/Utils/PrusaConnect.hpp"
#include <wx/utils.h>
#include <wx/webview.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

class wxWindow;

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

namespace Slic3r {
namespace GUI {

namespace {

bool is_prusa_connect_url(const wxString& url)
{
    wxString lower = url.Lower();
    lower.Trim(true).Trim(false);
    const wxString prefix = "https://connect.prusa3d.com";
    if (!lower.StartsWith(prefix))
        return false;
    if (lower.length() == prefix.length())
        return true;
    const wxChar separator = lower[prefix.length()];
    return separator == '/' || separator == '?' || separator == '#';
}

std::string make_prusa_connect_request_script(const char* kind,
                                              const std::string& organization_id,
                                              const std::string& printer_id,
                                              std::uint64_t request_id)
{
    const nlohmann::json request = {
        {"kind", kind},
        {"organizationId", organization_id},
        {"printerId", printer_id},
        {"requestId", request_id},
        {"query", prusa_connect_fff_graphql_query()},
    };

    // This runs in the authenticated Connect origin. The access token remains
    // inside WebView storage; only a credential-filtered response crosses the wx bridge.
    return "(async()=>{const q=" + request.dump() + R"JS(;
const send=(v)=>{if(window.wx&&typeof window.wx.postMessage==="function")window.wx.postMessage(JSON.stringify(Object.assign({},v,{command:"prusa_connect_result",requestId:q.requestId})));};
let timer=null;
try {
  if(location.hostname!=="connect.prusa3d.com"){send({kind:q.kind,ok:false,error:"Connect WebView is not active"});return;}
  const stored=(storage,key)=>{try{return storage.getItem(key)||"";}catch(e){return "";}};
  const token=(stored(sessionStorage,"auth.access_token")||stored(sessionStorage,"access_token")||stored(sessionStorage,"accessToken")||stored(sessionStorage,"authToken")||stored(localStorage,"auth.access_token")||stored(localStorage,"access_token")||stored(localStorage,"accessToken")||stored(localStorage,"authToken")||"").trim();
  const headers={Accept:"application/json"};
  if(token)headers.Authorization="Bearer "+token;
  const controller=new AbortController();
  timer=setTimeout(()=>controller.abort(),20000);
  let url=location.origin+"/app/printers";
  let init={method:"GET",headers:headers,credentials:"include",signal:controller.signal};
  if(q.kind==="filaments"){
    if(!q.organizationId||!q.printerId){clearTimeout(timer);send({kind:q.kind,ok:false,error:"Connect organization or printer is not selected"});return;}
    url="https://connect-api.prusa3d.com/graphql";
    headers["Content-Type"]="application/json";
    // Match the Connect web app's cross-origin GraphQL request. Do not include
    // cookies here: the API authorizes with the bearer header, and credentials
    // mode "include" would require CORS credentials from connect-api.
    headers["graphql-client-name"]="connect-web";
    init={method:"POST",headers:headers,credentials:"same-origin",body:JSON.stringify({query:q.query,variables:{organizationId:q.organizationId,printerId:q.printerId,withMaterialDetails:true},operationName:"GetPrinterFffTelemetry"}),signal:controller.signal};
  }
  const response=await fetch(url,init);
  const body=await response.text();
  clearTimeout(timer);
  const hasCredentialField=(value)=>{
    if(Array.isArray(value))return value.some(hasCredentialField);
    if(value&&typeof value==="object")return Object.entries(value).some(([key,child])=>/(token|password|secret|authorization|cookie|api[_-]?key)/i.test(key)||hasCredentialField(child));
    return false;
  };
  let parsedBody;
  try { parsedBody=JSON.parse(body); } catch(e) { parsedBody=null; }
  if((parsedBody&&hasCredentialField(parsedBody))||/(access[_-]?token|accessToken|refresh[_-]?token|refreshToken|api[_-]?key|apikey)/i.test(body)){send({kind:q.kind,ok:false,error:"Connect response contained credential fields"});return;}
  send({kind:q.kind,ok:response.ok,status:response.status,body:body});
} catch(e) { clearTimeout(timer); send({kind:q.kind,ok:false,error:e&&e.name==="AbortError"?"Connect request timed out":"Connect request failed"}); }
})();)JS";
}

} // namespace

#ifdef __linux__
// Workaround for #7210: WebKitGTK crashes on vue-resize's hidden <object> probe used by
// older Fluidd/Mainsail pages. Swap that <object> for a <div> shim at appendChild time
// and bridge resize events through a fake contentDocument.defaultView so vue-resize keeps
// working. Workaround proposed by @VittC.
static void inject_vue_resize_workaround(wxWebView *webView)
{
    webView->AddUserScript(
        "(function() {"
        "  'use strict';"
        "  function isVueResizeObject(el) {"
        "    return el && el.tagName === 'OBJECT'"
        "        && el.type === 'text/html'"
        "        && el.getAttribute('aria-hidden') === 'true'"
        "        && el.getAttribute('tabindex') === '-1';"
        "  }"
        "  function isResizeObserverParent(p) {"
        "    return p && p.classList && p.classList.contains('resize-observer');"
        "  }"
        "  function makeShim(orig, parentForRO) {"
        "    var shim = document.createElement('div');"
        "    shim.setAttribute('aria-hidden', 'true');"
        "    shim.setAttribute('tabindex', '-1');"
        "    shim.style.display = 'none';"
        "    var fakeWin = document.createElement('div');"
        "    var ro = null;"
        "    var origRemoveEL = fakeWin.removeEventListener.bind(fakeWin);"
        "    fakeWin.removeEventListener = function(type, fn, opts) {"
        "      origRemoveEL(type, fn, opts);"
        "      if (type === 'resize' && ro) { ro.disconnect(); ro = null; }"
        "    };"
        "    Object.defineProperty(shim, 'contentDocument', {"
        "      configurable: true,"
        "      get: function() { return { defaultView: fakeWin }; }"
        "    });"
        "    Object.defineProperty(shim, 'contentWindow', {"
        "      configurable: true,"
        "      get: function() { return fakeWin; }"
        "    });"
        "    if (typeof orig.onload === 'function') { shim.onload = orig.onload; }"
        "    queueMicrotask(function() {"
        "      if (parentForRO && typeof ResizeObserver !== 'undefined') {"
        "        ro = new ResizeObserver(function() {"
        "          fakeWin.dispatchEvent(new Event('resize'));"
        "        });"
        "        ro.observe(parentForRO);"
        "      }"
        "      if (typeof shim.onload === 'function') {"
        "        try { shim.onload(new Event('load')); } catch (e) {}"
        "      }"
        "      shim.dispatchEvent(new Event('load'));"
        "    });"
        "    return shim;"
        "  }"
        "  var origAppend = Node.prototype.appendChild;"
        "  Node.prototype.appendChild = function(child) {"
        "    if (isResizeObserverParent(this) && isVueResizeObject(child)) {"
        "      return origAppend.call(this, makeShim(child, this));"
        "    }"
        "    return origAppend.call(this, child);"
        "  };"
        "  var origInsertBefore = Node.prototype.insertBefore;"
        "  Node.prototype.insertBefore = function(child, ref) {"
        "    if (isResizeObserverParent(this) && isVueResizeObject(child)) {"
        "      return origInsertBefore.call(this, makeShim(child, this), ref);"
        "    }"
        "    return origInsertBefore.call(this, child, ref);"
        "  };"
        "  console.log('[vr-fix] vue-resize WebKitGTK patch active');"
        "})();",
        wxWEBVIEW_INJECT_AT_DOCUMENT_START
    );
}
#endif

// The strings of the page shown when the printer has no connection set up
// (resources/web/orca/missing_connection.html), keyed by its data-i18n ids.
static void add_page_strings(wxWebView *webView)
{
    const nlohmann::json strings = {
        {"printer_connection", _u8L("Printer Connection")},
        {"printer_connection_setup", _u8L("Please set up your printer connection to view the device.")},
    };
    webView->AddUserScript(wxString::FromUTF8(WebViewHostDialog::localized_strings_script(strings)));
}

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_browser(nullptr)
    , m_zoomFactor(100)
    , m_apikey()
    , m_url_deferred()
    , m_handler(std::make_unique<PrinterWebViewHandler>(*this))
    , m_prusa_connect_timeout_timer(this)
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

      // Create the webview
    create_browser();
    m_reset_on_show = WebView::NeedsRecreateOnShow();

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    /* m_browser->GetUserAgent() may lead crash
    if (wxGetApp().get_mode() == comDevelop) {
        wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("Backend: %s Version: %s", m_browser->GetClassInfo()->GetClassName(),
            wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("User Agent: %s", m_browser->GetUserAgent());
    }
    */

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
    Bind(wxEVT_TIMER, &PrinterWebView::OnPrusaConnectTimeout, this, m_prusa_connect_timeout_timer.GetId());

 }

PrinterWebView::~PrinterWebView()
{
    cancel_prusa_connect_request();
    m_prusa_connect_state->alive = false;
    ++m_prusa_connect_state->generation;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    m_handler.reset();

    // Destroy the webview
    if(m_browser){
        m_browser->Destroy();
        m_browser = nullptr;
    }


    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}

void PrinterWebView::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");
    add_page_strings(m_browser);

#ifdef __linux__
    inject_vue_resize_workaround(m_browser);

    auto cookiesPath = boost::filesystem::path(data_dir() + "/cache/cookies.db");
    auto wv = static_cast<WebKitWebView*>(m_browser->GetNativeBackend());
    auto wv_ctx = webkit_web_view_get_context(wv);
    auto cookieManager = webkit_web_context_get_cookie_manager(wv_ctx);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesPath.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#endif

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrinterWebView::OnNewWindow, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this);
    update_mode();
}

void PrinterWebView::reset_browser()
{
    cancel_prusa_connect_request();
    m_browser->Destroy(); // also removes it from the sizer
    create_browser();
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    Layout();

    // OnLoaded may have cleared m_url_deferred already, so requeue the last url for Show().
    m_api_key_script_installed = false;
    m_url_loaded = false;
    m_url_deferred = m_url;
}

void PrinterWebView::cancel_prusa_connect_request()
{
    m_prusa_connect_timeout_timer.Stop();
    m_prusa_connect_callback = {};
    m_prusa_connect_request_kind.clear();
    m_prusa_connect_request_generation = 0;
    ++m_prusa_connect_state->generation;
}

void PrinterWebView::restore_base_user_scripts()
{
    // RemoveAllUserScripts can remove the native wx handler on some backends,
    // while leaving it registered on others. Explicitly remove and re-add it so
    // the bridge has a deterministic state without duplicate WKWebView handlers.
    m_browser->RemoveAllUserScripts();
    m_browser->RemoveScriptMessageHandler("wx");
    if (m_browser->AddScriptMessageHandler("wx"))
        WebView::MarkScriptMessageHandlerAdded(m_browser);
    else
        BOOST_LOG_TRIVIAL(error) << "Could not restore the wx script message handler";
#ifdef __WIN32__
    m_browser->AddUserScript("window.wx = window.chrome.webview;");
#else
    m_browser->AddUserScript("window.wx = window.webkit.messageHandlers.wx;");
#endif
#ifdef __linux__
    inject_vue_resize_workaround(m_browser);
#endif
    add_page_strings(m_browser);
    m_api_key_script_installed = false;
}

// Same lookup as create_printer_webview_handler().
static PrintHostType active_host_type()
{
    if (wxGetApp().preset_bundle == nullptr)
        return htOctoPrint;
    return wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
}

void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    cancel_prusa_connect_request();
    if (m_api_key_script_installed && (m_apikey != apikey || is_prusa_connect_url(url)))
        restore_base_user_scripts();
//    this->Show();
//    this->Raise();
    const PrintHostType host_type = active_host_type();
    // Selecting the tab requests the same page again; navigating would reload it and drop its state.
    if (m_url_loaded && url == m_url && apikey == m_apikey && host_type == m_host_type)
        return;

    m_url = url;
    m_apikey = apikey;
    m_host_type = host_type;
    m_api_key_script_installed = false;
    m_url_loaded = false;
    m_handler = create_printer_webview_handler(*this);

    if (this->IsShown()) {
        //ORCA: m_url_deferred will be cleared on load success
        //m_url_deferred.clear();
        m_browser->LoadURL(url);
    } else {
        m_url_deferred = url;
    }
    //m_browser->SetFocus();
    UpdateState();
}

bool PrinterWebView::Show(bool show)
{
    if (show && std::exchange(m_reset_on_show, false))
        reset_browser();
    if (show && !m_url_deferred.empty()) {
        m_browser->LoadURL(m_url_deferred);
        //ORCA: m_url_deferred will be cleared on load success
        //m_url_deferred.clear();
    }
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

bool PrinterWebView::request_prusa_connect_printers(PrusaConnectResultCallback callback)
{
    if (!callback || m_prusa_connect_callback || m_browser == nullptr || m_url.empty() ||
        !is_prusa_connect_url(m_url) || !is_prusa_connect_url(m_browser->GetCurrentURL()))
        return false;
    m_prusa_connect_request_generation = ++m_prusa_connect_state->generation;
    m_prusa_connect_request_kind = "printers";
    m_prusa_connect_callback = std::move(callback);
    m_prusa_connect_timeout_timer.StartOnce(25000);
    if (!WebView::RunScript(m_browser, wxString::FromUTF8(make_prusa_connect_request_script("printers", {}, {}, m_prusa_connect_request_generation)))) {
        m_prusa_connect_callback = {};
        m_prusa_connect_request_kind.clear();
        m_prusa_connect_request_generation = 0;
        ++m_prusa_connect_state->generation;
        return false;
    }
    return true;
}

bool PrinterWebView::request_prusa_connect_filaments(const std::string& organization_id,
                                                     const std::string& printer_id,
                                                     PrusaConnectResultCallback callback)
{
    if (!callback || m_prusa_connect_callback || m_browser == nullptr || m_url.empty() ||
        !is_prusa_connect_url(m_url) || !is_prusa_connect_url(m_browser->GetCurrentURL()) || organization_id.empty() || printer_id.empty())
        return false;
    m_prusa_connect_request_generation = ++m_prusa_connect_state->generation;
    m_prusa_connect_request_kind = "filaments";
    m_prusa_connect_callback = std::move(callback);
    m_prusa_connect_timeout_timer.StartOnce(25000);
    if (!WebView::RunScript(m_browser,
                            wxString::FromUTF8(make_prusa_connect_request_script("filaments", organization_id, printer_id,
                                                                                   m_prusa_connect_request_generation)))) {
        m_prusa_connect_callback = {};
        m_prusa_connect_request_kind.clear();
        m_prusa_connect_request_generation = 0;
        ++m_prusa_connect_state->generation;
        return false;
    }
    return true;
}

void PrinterWebView::OnPrusaConnectTimeout(wxTimerEvent& event)
{
    (void) event;
    if (!m_prusa_connect_callback)
        return;

    const std::string kind = m_prusa_connect_request_kind;
    const std::uint64_t request_id = m_prusa_connect_request_generation;
    auto callback = std::move(m_prusa_connect_callback);
    m_prusa_connect_request_kind.clear();
    m_prusa_connect_request_generation = 0;
    ++m_prusa_connect_state->generation;
    if (wxGetApp().is_closing())
        return;

    const nlohmann::json envelope = {
        {"command", "prusa_connect_result"},
        {"kind", kind},
        {"requestId", request_id},
        {"ok", false},
        {"error", "Connect request timed out"},
    };
    callback(envelope.dump());
}

void PrinterWebView::update_mode()
{
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    // Connect authenticates through its own OAuth/PKCE flow. Never inject a
    // printer-host API key into that origin, even if an older caller supplied
    // one while switching host types.
    if (m_api_key_script_installed || m_apikey.IsEmpty() || is_prusa_connect_url(m_url) ||
        (m_browser != nullptr && is_prusa_connect_url(m_browser->GetCurrentURL())))
        return;
    const std::string api_key = m_apikey.ToUTF8().data();
    const wxString escaped_api_key = wxString::FromUTF8(nlohmann::json(api_key).dump());
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = %s;
            return originalFetch(input, init);
        };
    }
)",
                                       escaped_api_key);
    restore_base_user_scripts();
    m_browser->AddUserScript(script);
    m_api_key_script_installed = true;
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent& evt)
{
    // The web view's initial empty document (about:blank on WebKit) is not the requested page.
    if (evt.GetURL().IsEmpty() || evt.GetURL() == "about:blank")
        return;
    cancel_prusa_connect_request();
    //ORCA: url loaded successfully, safe to clear
    m_url_deferred.clear();
    const bool connect_document = is_prusa_connect_url(evt.GetURL()) ||
                                  (m_browser != nullptr && is_prusa_connect_url(m_browser->GetCurrentURL()));
    if (connect_document && m_api_key_script_installed) {
        // A user-driven navigation can reach Connect without going through
        // load_url(). Remove the host API-key script and reload before any
        // Connect page code can make requests with the stale wrapper installed.
        m_url_loaded = false;
        restore_base_user_scripts();
        m_browser->Reload();
        return;
    }
    m_url_loaded = true;
    SendAPIKey();
  
    if (m_handler != nullptr) {
        m_handler->on_loaded(evt);
        return;
    }
}

void PrinterWebView::OnNewWindow(wxWebViewEvent& evt)
{
  const wxString url = evt.GetURL();
  if (!url.empty())
    wxLaunchDefaultBrowser(url);
  evt.Veto();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt)
{
  const std::string message = evt.GetString().ToUTF8().data();
  const nlohmann::json payload = nlohmann::json::parse(message, nullptr, false);
  const auto command = payload.is_object() ? payload.find("command") : payload.end();
  const auto kind = payload.is_object() ? payload.find("kind") : payload.end();
  const auto request_id = payload.is_object() ? payload.find("requestId") : payload.end();
  const bool trusted_connect_document = m_browser != nullptr &&
                                        is_prusa_connect_url(m_browser->GetCurrentURL());
  if (trusted_connect_document && !payload.is_discarded() && payload.is_object() && command != payload.end() && command->is_string() &&
      command->get<std::string>() == "prusa_connect_result" && kind != payload.end() && kind->is_string() &&
      kind->get<std::string>() == m_prusa_connect_request_kind && request_id != payload.end() &&
      request_id->is_number_unsigned() && request_id->get<std::uint64_t>() == m_prusa_connect_request_generation &&
      m_prusa_connect_callback) {
      const auto ok = payload.find("ok");
      const auto status = payload.find("status");
      const auto body = payload.find("body");
      const auto error = payload.find("error");
      if (ok == payload.end() || !ok->is_boolean() ||
          (status != payload.end() && !status->is_number_integer() && !status->is_number_unsigned()) ||
          (ok->get<bool>() && (body == payload.end() || !body->is_string())) ||
          (!ok->get<bool>() && error != payload.end() && !error->is_string()))
          return;

      m_prusa_connect_timeout_timer.Stop();
      auto callback = std::move(m_prusa_connect_callback);
      m_prusa_connect_request_kind.clear();
      m_prusa_connect_request_generation = 0;
      const auto state = m_prusa_connect_state;
      const auto generation = state->generation;
      if (callback && !wxGetApp().is_closing()) {
          wxGetApp().CallAfter([state, generation, callback = std::move(callback), message]() mutable {
              if (state->alive && state->generation == generation && !wxGetApp().is_closing())
                  callback(message);
          });
      }
      return;
  }
  if (!payload.is_discarded() && payload.is_object() && command != payload.end() && command->is_string() &&
      command->get<std::string>() == "prusa_connect_result")
      return;
  if (m_handler != nullptr)
    m_handler->on_script_message(evt);
}


} // GUI
} // Slic3r
