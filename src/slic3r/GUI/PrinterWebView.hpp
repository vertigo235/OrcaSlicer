#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/setup.h>
#include <wx/event.h>
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "Lazy.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {
namespace GUI {

class PrinterWebViewHandler;


class PrinterWebView : public wxPanel, public LazyInstance<PrinterWebView> {
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnNewWindow(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();

    // Execute a Connect request inside the authenticated Connect WebView. The
    // callback receives a non-sensitive JSON envelope containing only the HTTP
    // status and response body; OAuth tokens never cross this boundary.
    using PrusaConnectResultCallback = std::function<void(std::string)>;
    bool request_prusa_connect_printers(PrusaConnectResultCallback callback);
    bool request_prusa_connect_filaments(const std::string& organization_id,
                                         const std::string& printer_id,
                                         PrusaConnectResultCallback callback);

    bool Show(bool show = true) override;

private:
    friend class PrinterWebViewHandler;

    void SendAPIKey();
    void restore_base_user_scripts();
    void cancel_prusa_connect_request();
    void OnPrusaConnectTimeout(wxTimerEvent& event);
    void create_browser();
    void reset_browser();

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    bool m_api_key_script_installed = false;
    // Last url passed to load_url(), reloaded after reset_browser().
    wxString m_url;
    wxString m_url_deferred;
    // Host type the page was loaded for; it selects the handler, so a change needs a reload.
    PrintHostType m_host_type{htOctoPrint};
    // True once the page for m_url has finished loading in m_browser.
    bool m_url_loaded{false};
    std::unique_ptr<PrinterWebViewHandler> m_handler;
    struct PrusaConnectRequestState {
        bool alive = true;
        std::uint64_t generation = 0;
    };

    PrusaConnectResultCallback m_prusa_connect_callback;
    std::string m_prusa_connect_request_kind;
    std::uint64_t m_prusa_connect_request_generation = 0;
    std::shared_ptr<PrusaConnectRequestState> m_prusa_connect_state = std::make_shared<PrusaConnectRequestState>();
    wxTimer m_prusa_connect_timeout_timer;
    bool m_reset_on_show{false};

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
