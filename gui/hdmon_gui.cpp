// Hard Drive Monitor desktop app (hdmon-gui): C++/wxWidgets port of the VB.NET DiskMonitor tray app.
// Tray circle in the worst drive's colour, drive list window, Details, Options (incl. Updates), About,
// danger alerts. Scanning, scoring, history, alerts, config and updates all come from hdmon_core.
// hdmon headers first: <windows.h> (pulled in by wx) defines `interface`, a Drive field name.
#include "alerts.hpp"
#include "autostart.hpp"
#include "collect.hpp"
#include "config.hpp"
#include "gui_model.hpp"
#include "raid.hpp"
#include "render.hpp"
#include "update.hpp"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/notifmsg.h>
#include <wx/snglinst.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/taskbar.h>
#include <wx/hyperlink.h>
#include <wx/mstream.h>
#include <wx/utils.h>

#include <atomic>
#ifndef __WXMSW__
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#endif
#include <cmath>
#include <ctime>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>


#include "app_icon_png.h"

using namespace hdmon;
namespace g = hdmon::gui;

static const char* GUI_VERSION = HDMON_GUI_VERSION;   // "Beta V x.y.z" (same as the CLI)
static wxString U(const std::string& s) { return wxString::FromUTF8(s.c_str()); }
static std::string S(const wxString& s) { return std::string(s.utf8_str()); }
static wxColour C(const std::string& hex) {
    auto c = hdmon::palette::to_rgb(hex.c_str());   // exact palette RGB, no theme adjustment
    return wxColour(c.r, c.g, c.b);
}

// Plain filled circle, exactly like VB TrayIcons.Circle: FillEllipse(2, 2, 28, 28) on a transparent
// 32 px bitmap with SmoothingMode.AntiAlias. Colour is the exact palette RGB; only edge alpha varies.
static wxBitmap circle_bitmap(const std::string& hex) {
    namespace p = hdmon::palette;
    wxColour c = C(hex);
    const int size = p::ICON_SIZE;
    wxImage img(size, size);
    img.InitAlpha();
    const double r = p::CIRCLE_D / 2.0, cx = p::CIRCLE_X + r, cy = p::CIRCLE_Y + r;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double a = std::min(1.0, std::max(0.0, r + 0.5 - std::sqrt(dx * dx + dy * dy)));
            img.SetRGB(x, y, c.Red(), c.Green(), c.Blue());
            img.SetAlpha(x, y, static_cast<unsigned char>(a * 255 + 0.5));
        }
    return wxBitmap(img);
}
static wxIcon circle_icon(const std::string& status) {
    wxIcon ic;
    ic.CopyFromBitmap(circle_bitmap(g::status_color(status)));
    return ic;
}
static wxIcon app_icon() {
#ifdef __WXMSW__
    return wxICON(appicon);
#else
    wxMemoryInputStream in(APP_ICON_PNG, sizeof APP_ICON_PNG);
    wxImage img(in, wxBITMAP_TYPE_PNG);
    wxIcon ic;
    if (img.IsOk()) ic.CopyFromBitmap(wxBitmap(img));
    return ic;
#endif
}

struct ScanOutcome {
    std::vector<Result> results;
    std::vector<RaidArray> arrays;
    json meta = json::object();
    std::string error;
    std::vector<Alert> alerts;
    std::string finished;   // local HH:MM
};

class HdmonApp;
wxDECLARE_APP(HdmonApp);

// ------------------------------------------------------------------ dialogs
// VB SmallDialog: fixed, no min/max, centred.
class SmallDialog : public wxDialog {
public:
    SmallDialog(wxWindow* parent, const wxString& title)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE) {
        SetIcon(app_icon());
    }
};

class DetailsDialog : public SmallDialog {
public:
    DetailsDialog(wxWindow* parent, const Result& r)
        : DetailsDialog(parent, "Details - " + g::drive_name(r.first) + " " + r.first.model.value_or(""),
                        g::detail_rows(r.first, r.second), detail_view(r.first, r.second, Painter("never"))) {}
    DetailsDialog(wxWindow* parent, const RaidArray& a, const std::vector<Result>& results)
        : DetailsDialog(parent, "Details - RAID " + a.name, g::array_detail_rows(a, results),
                        raid_view({a}, results, Painter("never"))) {}
    DetailsDialog(wxWindow* parent, const std::string& title, const std::vector<std::pair<std::string, std::string>>& rows,
                  const std::string& report)
        : SmallDialog(parent, U(title)) {
        auto* top = new wxBoxSizer(wxVERTICAL);
        auto* nb = new wxNotebook(this, wxID_ANY);
        auto* page = new wxPanel(nb);
        auto* ps = new wxBoxSizer(wxVERTICAL);
        auto* lv = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(660, 270)),
                                  wxLC_REPORT | wxLC_NO_HEADER | wxLC_SINGLE_SEL);
        lv->AppendColumn("", wxLIST_FORMAT_LEFT, FromDIP(210));
        lv->AppendColumn("", wxLIST_FORMAT_LEFT, FromDIP(440));
        long i = 0;
        std::string notes;
        for (auto& [k, v] : rows) {
            if (k == "Notes") { notes = v; continue; }   // long text: shown wrapped below the list
            lv->InsertItem(i, U(k));
            lv->SetItem(i, 1, U(v));
            ++i;
        }
        ps->Add(lv, 1, wxEXPAND);
        auto* nt = new wxStaticText(page, wxID_ANY, U(notes.empty() ? std::string() : "Notes: " + replace_all(notes, "; ", "\n")));
        nt->Wrap(FromDIP(560));
        ps->Add(nt, 0, wxALL, FromDIP(8));
        page->SetSizer(ps);
        nb->AddPage(page, "Summary");
        auto* txt = new wxTextCtrl(nb, wxID_ANY, U(report), wxDefaultPosition,
                                   wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxHSCROLL);
        txt->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
        nb->AddPage(txt, "Full report");
        top->Add(nb, 1, wxEXPAND | wxALL, FromDIP(10));
        auto* close = new wxButton(this, wxID_OK, "Close");
        top->Add(close, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(10));
        SetAffirmativeId(wxID_OK);
        SetEscapeId(wxID_OK);
        SetSizerAndFit(top);
        SetSize(FromDIP(wxSize(740, 500)));
        CentreOnScreen();
    }
};

class AboutDialog : public SmallDialog {
public:
    AboutDialog(wxWindow* parent, bool smartctl_used) : SmallDialog(parent, "About Hard Drive Monitor") {
        auto* top = new wxBoxSizer(wxVERTICAL);
        auto* head = new wxBoxSizer(wxHORIZONTAL);
        wxIcon ic = app_icon();
        if (ic.IsOk()) {
            wxBitmap bmp;
            bmp.CopyFromIcon(ic);
            if (bmp.IsOk()) {
                wxImage im = bmp.ConvertToImage().Rescale(FromDIP(40), FromDIP(40), wxIMAGE_QUALITY_HIGH);
                head->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(im)), 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
            }
        }
        auto* title = new wxStaticText(this, wxID_ANY, U(std::string("Hard Drive Monitor ") + GUI_VERSION));
        title->SetFont(wxFontInfo(14).Bold());
        head->Add(title, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(head, 0, wxALL, FromDIP(16));
        top->Add(new wxStaticText(this, wxID_ANY, U("\u00A9 2026 Richard Reeves")), 0, wxLEFT | wxRIGHT, FromDIP(18));
        top->Add(new wxStaticText(this, wxID_ANY,
                                  "SMART drive health for Windows, macOS and Linux.\n"
                                  "Successor of Disk Monitor (VB.NET, Beta 0.05)."),
                 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
        top->Add(new wxStaticText(this, wxID_ANY,
                                  "SMART data is read natively through the operating system\n"
                                  "(no extra tools needed). RAID arrays: Linux md, Storage Spaces,\n"
                                  "Intel RST, AppleRAID and hardware RAID controllers."),
                 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
        if (smartctl_used) {   // credit smartmontools only when the optional fallback was actually used
            top->Add(new wxStaticText(this, wxID_ANY,
                                      "Some drives were read with the optional smartmontools (smartctl)\n"
                                      "fallback, which is free software licensed under the GNU GPL,\n"
                                      "version 2 or later."),
                     0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
            top->Add(new wxHyperlinkCtrl(this, wxID_ANY, "www.smartmontools.org", "https://www.smartmontools.org"), 0,
                     wxLEFT | wxRIGHT, FromDIP(18));
        }
        top->Add(new wxHyperlinkCtrl(this, wxID_ANY, "github.com/rreeves5/hard-drive-monitor",
                                     "https://github.com/rreeves5/hard-drive-monitor"),
                 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
        auto* ok = new wxButton(this, wxID_OK, "OK");
        ok->SetDefault();
        top->Add(ok, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));
        SetSizerAndFit(top);
        CentreOnScreen();
    }
};

// VB AlertForm: red header, always on top, modeless.
class AlertDialog : public wxDialog {
public:
    AlertDialog(const std::vector<Alert>& alerts)
        : wxDialog(nullptr, wxID_ANY, "Hard Drive Monitor", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP) {
        SetIcon(circle_icon("CRITICAL"));
        auto* top = new wxBoxSizer(wxVERTICAL);
        auto* head = new wxPanel(this);
        head->SetBackgroundColour(C(g::status_color("CRITICAL")));
        auto* hs = new wxBoxSizer(wxHORIZONTAL);
        auto* ht = new wxStaticText(head, wxID_ANY, "Drive in danger");
        ht->SetForegroundColour(*wxWHITE);
        ht->SetFont(wxFontInfo(12).Bold());
        hs->Add(ht, 0, wxALL, FromDIP(10));
        head->SetSizer(hs);
        top->Add(head, 0, wxEXPAND);
        std::string body = alert_text(alerts);
        auto* bt = new wxStaticText(this, wxID_ANY, U(body));
        bt->Wrap(FromDIP(420));
        top->Add(bt, 1, wxALL | wxEXPAND, FromDIP(14));
        auto* ok = new wxButton(this, wxID_OK, "OK");
        top->Add(ok, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(12));
        SetSizerAndFit(top);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Destroy(); }, wxID_OK);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });
        CentreOnScreen();
    }
};

class OptionsDialog : public SmallDialog {
public:
    OptionsDialog(wxWindow* parent, const g::GuiSettings& s, bool autostart)
        : SmallDialog(parent, "Options") {
        auto* top = new wxBoxSizer(wxVERTICAL);
        int pad = FromDIP(12);
        run_ = new wxCheckBox(this, wxID_ANY, "Start at login");
        run_->SetValue(autostart);
        alert_ = new wxCheckBox(this, wxID_ANY, "Alert me when a drive is in danger");
        alert_->SetValue(s.alert_on_danger);
        top->Add(run_, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        top->Add(alert_, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        auto* lv = new wxBoxSizer(wxHORIZONTAL);
        lv->Add(new wxStaticText(this, wxID_ANY, "Alert on:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        level_ = new wxChoice(this, wxID_ANY);
        level_->Append("Yellow (warning)");
        level_->Append("Red (critical only)");
        level_->SetSelection(s.critical_only ? 1 : 0);
        lv->Add(level_, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(lv, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        auto* wr = new wxBoxSizer(wxHORIZONTAL);
        wr->Add(new wxStaticText(this, wxID_ANY, "Warn when health drops by:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        drop_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 99,
                               s.warn_drop_percent);
        wr->Add(drop_, 0, wxALIGN_CENTER_VERTICAL);
        wr->Add(new wxStaticText(this, wxID_ANY, "%"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        top->Add(wr, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, "Scan every"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        interval_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 1440,
                                   s.scan_interval_minutes);
        row->Add(interval_, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(new wxStaticText(this, wxID_ANY, "minutes"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        top->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, pad);

        auto* box = new wxStaticBoxSizer(wxVERTICAL, this, "Updates");
        wxWindow* bp = box->GetStaticBox();
        check_ = new wxCheckBox(bp, wxID_ANY, "Check for updates automatically (once a day)");
        check_->SetValue(s.auto_check);
        box->Add(check_, 0, wxALL, FromDIP(6));
        auto* crow = new wxBoxSizer(wxHORIZONTAL);
        auto* now = new wxButton(bp, wxID_ANY, "Check now");
        status_ = new wxStaticText(bp, wxID_ANY, U(std::string("You have ") + GUI_VERSION + "."));
        crow->Add(now, 0, wxALIGN_CENTER_VERTICAL);
        crow->Add(status_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        box->Add(crow, 0, wxEXPAND | wxALL, FromDIP(6));
        auto* note = new wxStaticText(bp, wxID_ANY,
                                      "Off by default; it only notifies. Updates are installed only when you\n"
                                      "click Check now, and are checked with SHA-256 first.");
        note->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        box->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        top->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
        now->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CheckNow(); });

        auto* btns = new wxStdDialogButtonSizer();
        auto* ok = new wxButton(this, wxID_OK, "OK");
        ok->SetDefault();
        btns->AddButton(ok);
        btns->AddButton(new wxButton(this, wxID_CANCEL, "Cancel"));
        btns->Realize();
        top->Add(btns, 0, wxALIGN_RIGHT | wxALL, pad);
        SetSizerAndFit(top);
        CentreOnScreen();
    }

    bool RunAtStart() const { return run_->GetValue(); }
    g::GuiSettings Result() const {
        g::GuiSettings s;
        s.alert_on_danger = alert_->GetValue();
        s.critical_only = level_->GetSelection() == 1;
        s.warn_drop_percent = drop_->GetValue();
        s.scan_interval_minutes = interval_->GetValue();
        s.auto_check = check_->GetValue();
        return s;
    }

private:
    wxCheckBox *run_, *alert_, *check_;
    wxSpinCtrl* interval_;
    wxSpinCtrl* drop_;
    wxChoice* level_;
    wxStaticText* status_;

    void SetStatus(const std::string& t) {
        status_->SetLabel(U(t));
        Layout();
        Fit();
    }

    void CheckNow() {
        ReleaseInfo rel;
        CheckResult r;
        {
            wxBusyCursor busy;
            SetStatus("Checking GitHub...");
            wxYield();
            r = check_for_update(GUI_VERSION, &rel);
        }
        if (!r.ok) { SetStatus("Check failed: " + r.error); return; }
        if (!r.update_available) {
            SetStatus(std::string("Up to date (") + GUI_VERSION + "; latest release " + r.latest + ").");
            return;
        }
        SetStatus("Update available: " + r.latest);
        if (current_os() == OS::MacOS) {
            if (wxMessageBox(U(r.latest + " is available (you have " + GUI_VERSION + ").\n\nOpen the download page?"),
                             "Hard Drive Monitor update", wxYES_NO | wxICON_INFORMATION, this) == wxYES)
                wxLaunchDefaultBrowser(U(r.url));
            return;
        }
        if (wxMessageBox(U(r.latest + " is available (you have " + GUI_VERSION + ").\n\n"
                           "Download it, verify its SHA-256 checksum and install it now?"),
                         "Hard Drive Monitor update", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
            return;
        InstallResult ir;
        {
            wxBusyCursor busy;
            SetStatus("Downloading and verifying...");
            wxYield();
            ir = install_update(rel, UpdateFlavor::Gui);
        }
        if (!ir.ok) {
            SetStatus("Update failed.");
            wxMessageBox(U("The update was not installed:\n" + ir.message), "Hard Drive Monitor update", wxOK | wxICON_ERROR, this);
            return;
        }
        SetStatus(ir.message);
        for (auto& w : ir.warnings) ir.message += "\n\n" + w;
        if (wxMessageBox(U(ir.message + "\n\nRestart Hard Drive Monitor now to use it?"), "Hard Drive Monitor update",
                         wxYES_NO | wxICON_INFORMATION, this) == wxYES)
            EndModal(ID_RESTART);
    }

public:
    static const int ID_RESTART = wxID_HIGHEST + 77;
};

// ------------------------------------------------------------------ main window
class MainFrame : public wxFrame {
public:
    MainFrame() : wxFrame(nullptr, wxID_ANY, U(std::string("Hard Drive Monitor ") + GUI_VERSION)) {
        SetIcon(app_icon());
        auto* panel = new wxPanel(this);
        auto* top = new wxBoxSizer(wxVERTICAL);
        list_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->AppendColumn("Drive", wxLIST_FORMAT_LEFT, FromDIP(150));
        list_->AppendColumn("Model", wxLIST_FORMAT_LEFT, FromDIP(270));
        list_->AppendColumn("Size", wxLIST_FORMAT_RIGHT, FromDIP(90));
        list_->AppendColumn("Temp", wxLIST_FORMAT_RIGHT, FromDIP(70));
        list_->AppendColumn("Health", wxLIST_FORMAT_RIGHT, FromDIP(350));
        top->Add(list_, 1, wxEXPAND);
        panel->SetSizer(top);
        CreateStatusBar(1);
        SetClientSize(FromDIP(wxSize(980, 300)));
        CentreOnScreen();

        list_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { e.Skip(); CallAfter([this] { FitColumns(); }); });
        Bind(wxEVT_SHOW, [this](wxShowEvent& e) { e.Skip(); if (e.IsShown()) CallAfter([this] { FitColumns(); }); });
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
            long i = e.GetIndex();
            if (i >= 0 && i < static_cast<long>(rows_.size()) && rows_[i].kind == g::DisplayRow::ARRAY) Toggle(i);
            else ShowDetails();
        });
        // A click on the ▼/▶ arrow of an array row collapses or expands it.
        list_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            int flags = 0;
            long i = list_->HitTest(e.GetPosition(), flags);
            e.Skip();
            if (i >= 0 && e.GetPosition().x < FromDIP(22)) CallAfter([this, i] { Toggle(i); });
        });
        list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& e) {
            long i = e.GetIndex();
            wxMenu m;
            m.Append(wxID_INFO, "Details");
            m.Bind(wxEVT_MENU, [this](wxCommandEvent&) { ShowDetails(); }, wxID_INFO);
            if (i >= 0 && i < static_cast<long>(rows_.size()) && rows_[i].kind == g::DisplayRow::ARRAY) {
                m.Append(wxID_VIEW_LIST, rows_[i].expanded ? "Collapse" : "Expand");
                m.Bind(wxEVT_MENU, [this, i](wxCommandEvent&) { Toggle(i); }, wxID_VIEW_LIST);
            }
            PopupMenu(&m);
        });
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    }

    void ShowScan(const std::vector<Result>& results, const std::vector<RaidArray>& arrays, bool have, bool scanning,
                  const std::string& status) {
        SetTitle(U(std::string("Hard Drive Monitor ") + GUI_VERSION + (scanning ? " - scanning..." : "")));
        SetStatusText(U(status));
        if (!have) return;
        results_ = results;
        arrays_ = arrays;
        Fill();
    }

    // Array rows: bold, ▼/▶ arrow, the array's status colour, state text ("Degraded", "Rebuilding 45.0%") in
    // the Health column; members indented under it. Same palette as plain drive rows.
    void Fill() {
        long keep = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        rows_ = g::display_rows(results_, arrays_, collapsed_);
        list_->Freeze();
        list_->DeleteAllItems();
        wxFont bold = list_->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        for (long i = 0; i < static_cast<long>(rows_.size()); ++i) {
            const auto& r = rows_[i];
            list_->InsertItem(i, U(r.cells[0]));
            for (int c = 1; c < 5; ++c) list_->SetItem(i, c, U(r.cells[c]));
            list_->SetItemBackgroundColour(i, C(g::row_tint(r.status)));
            list_->SetItemTextColour(i, C(hdmon::palette::ROW_TEXT));
            if (r.bold) list_->SetItemFont(i, bold);
        }
        if (keep >= 0 && keep < list_->GetItemCount()) list_->SetItemState(keep, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        list_->Thaw();
        FitColumns();
    }

    // Columns always fill the list exactly: no horizontal scrollbar, no blank gap right of Health.
    void FitColumns() {
        int avail = list_->GetClientSize().GetWidth();     // client area: excludes a visible vertical scrollbar
        if (avail <= 0) return;
        const int pad = FromDIP(14);
        wxFont bold = list_->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        std::vector<int> mins(5, 0), hdr(5, 0);
        for (int c = 0; c < 5; ++c) {
            wxListItem it;
            it.SetMask(wxLIST_MASK_TEXT);
            list_->GetColumn(c, it);
            hdr[c] = list_->GetTextExtent(it.GetText()).GetWidth() + pad;
        }
        wxClientDC dc(list_);
        for (const auto& r : rows_) {
            dc.SetFont(r.bold ? bold : list_->GetFont());
            for (int c = 0; c < 5; ++c) mins[c] = std::max(mins[c], dc.GetTextExtent(U(r.cells[c])).GetWidth() + pad);
        }
        auto w = g::fit_column_widths(mins, hdr, avail, 1);                // Model takes the slack
        for (int c = 0; c < 5; ++c)
            if (list_->GetColumnWidth(c) != w[c]) list_->SetColumnWidth(c, w[c]);
    }

    void Toggle(long i) {
        if (i < 0 || i >= static_cast<long>(rows_.size()) || rows_[i].kind != g::DisplayRow::ARRAY) return;
        const std::string& id = arrays_[rows_[i].array].id;
        if (!collapsed_.erase(id)) collapsed_.insert(id);
        Fill();
    }

    bool SelectFirstArray() {
        for (long i = 0; i < static_cast<long>(rows_.size()); ++i)
            if (rows_[i].kind == g::DisplayRow::ARRAY) {
                list_->SetItemState(i, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                return true;
            }
        return false;
    }
    void SelectFirst() {
        if (list_->GetItemCount() > 0) list_->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }
    void ShowDetails();

private:
    wxListCtrl* list_;
    std::vector<Result> results_;
    std::vector<RaidArray> arrays_;
    std::vector<g::DisplayRow> rows_;
    std::set<std::string> collapsed_;
    void OnClose(wxCloseEvent&);
};

// ------------------------------------------------------------------ tray
enum { ID_OPEN = wxID_HIGHEST + 1, ID_SCAN, ID_OPTIONS, ID_ABOUT, ID_EXIT };
class TrayIcon : public wxTaskBarIcon {
public:
    TrayIcon();
    wxMenu* MakeMenu();

protected:
    wxMenu* CreatePopupMenu() override { return MakeMenu(); }
};

// ------------------------------------------------------------------ app
class HdmonApp : public wxApp {
public:
    bool OnInit() override;
    int OnExit() override;
#ifdef __WXOSX__
    void MacReopenApp() override { ShowWindow(); }
#endif

    void ShowWindow();
    void ShowOptions();
    void ShowAbout();
    void ScanNow();
    void ExitApp();
    void ShowTrayMenu() {
        if (tray_) {
            std::unique_ptr<wxMenu> m(tray_->MakeMenu());
            tray_->PopupMenu(m.get());
        }
    }

private:
    std::vector<std::string> from_json_;
    std::string config_, db_, open_;
    bool show_ = false;
    std::unique_ptr<wxSingleInstanceChecker> single_;
    TrayIcon* tray_ = nullptr;
    MainFrame* frame_ = nullptr;
    wxTimer timer_;
    std::thread worker_;
    std::atomic<bool> scanning_{false};
    bool dialog_open_ = false;
    bool have_ = false;
    ScanOutcome last_;
    g::GuiSettings settings_;

    json Config() const {
        try { return load_config(config_); } catch (...) { return default_config(); }
    }
    void ApplyTimer() { timer_.Start(settings_.scan_interval_minutes * 60 * 1000); }
    std::string StatusLine() const;
    void Refresh();
    void OnScanDone(ScanOutcome out);
    void Notify(const std::string& title, const std::string& body, bool error);
    void Restart();
    wxWindow* Owner() { return frame_ && frame_->IsShown() ? frame_ : nullptr; }
};

wxIMPLEMENT_APP_NO_MAIN(HdmonApp);

TrayIcon::TrayIcon() {
    Bind(wxEVT_TASKBAR_LEFT_DCLICK, [](wxTaskBarIconEvent&) { wxGetApp().CallAfter([] { wxGetApp().ShowWindow(); }); });
#ifdef __WXGTK__
    Bind(wxEVT_TASKBAR_LEFT_DOWN, [](wxTaskBarIconEvent&) { wxGetApp().CallAfter([] { wxGetApp().ShowWindow(); }); });
#endif
}

wxMenu* TrayIcon::MakeMenu() {
    auto* m = new wxMenu;
    auto* open = new wxMenuItem(m, ID_OPEN, "Open");
#ifdef __WXMSW__
    wxFont f = open->GetFont();
    f.SetWeight(wxFONTWEIGHT_BOLD);
    open->SetFont(f);
#endif
    m->Append(open);
    m->Append(ID_SCAN, "Scan Now");
    m->Append(ID_OPTIONS, "Options");
    m->Append(ID_ABOUT, "About");
    m->AppendSeparator();
    m->Append(ID_EXIT, "Exit");
    m->Bind(wxEVT_MENU, [](wxCommandEvent& e) {
        int id = e.GetId();
        wxGetApp().CallAfter([id] {
            auto& app = wxGetApp();
            if (id == ID_OPEN) app.ShowWindow();
            else if (id == ID_SCAN) app.ScanNow();
            else if (id == ID_OPTIONS) app.ShowOptions();
            else if (id == ID_ABOUT) app.ShowAbout();
            else if (id == ID_EXIT) app.ExitApp();
        });
    });
    return m;
}

void MainFrame::ShowDetails() {
    long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= static_cast<long>(rows_.size())) return;
    const auto& r = rows_[sel];
    if (r.kind == g::DisplayRow::ARRAY || (r.kind == g::DisplayRow::MEMBER && r.result < 0)) {
        DetailsDialog dlg(this, arrays_[r.array], results_);
        dlg.ShowModal();
        return;
    }
    if (r.result < 0) return;
    DetailsDialog dlg(this, results_[r.result]);
    dlg.ShowModal();
}
void MainFrame::OnClose(wxCloseEvent& e) {
    // Like the VB app: closing the window leaves the tray icon running. Without a tray, closing quits.
    if (wxTaskBarIcon::IsAvailable() && e.CanVeto()) { Hide(); e.Veto(); return; }
    wxGetApp().CallAfter([] { wxGetApp().ExitApp(); });
    e.Skip();
}

// ------------------------------------------------------------------ single instance: a new start kills the old copy
static std::string pid_path() { return data_dir() + "/hdmon-gui.pid"; }

static void kill_old_instance() {
    long pid = g::parse_pid_file(read_file(pid_path()));
    if (pid <= 0 || pid == static_cast<long>(wxGetProcessId())) return;
    std::string image;
#ifdef __WXMSW__
    HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!p) return;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(p, 0, buf, &n) && g::is_gui_image(S(wxString(buf, n)))) {
        TerminateProcess(p, 1);
        WaitForSingleObject(p, 2000);
    }
    CloseHandle(p);
#else
    char buf[4096];
#ifdef __APPLE__
    int n = proc_pidpath(static_cast<int>(pid), buf, sizeof buf);
#else
    ssize_t n = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), buf, sizeof buf);
#endif
    if (n <= 0 || !g::is_gui_image(std::string(buf, static_cast<size_t>(n)))) return;
    kill(static_cast<pid_t>(pid), SIGTERM);
    for (int i = 0; i < 20 && kill(static_cast<pid_t>(pid), 0) == 0; ++i) wxMilliSleep(100);
    kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

bool HdmonApp::OnInit() {
    SetAppName("hard-drive-monitor");
    SetAppDisplayName("Hard Drive Monitor");
    for (int i = 1; i < argc; ++i) {
        std::string a = S(argv[i]);
        auto next = [&]() -> std::string { return i + 1 < argc ? S(argv[++i]) : std::string(); };
        if (a == "--from-json") from_json_.push_back(next());
        else if (a == "--config") config_ = next();
        else if (a == "--db") db_ = next();
        else if (a == "--open") open_ = next();
        else if (a == "--show") show_ = true;
    }
    if (config_.empty()) config_ = config_path();
    if (db_.empty()) db_ = default_db();
    wxInitAllImageHandlers();

    single_ = std::make_unique<wxSingleInstanceChecker>("hard-drive-monitor-gui-" + wxGetUserId());
    if (from_json_.empty() && single_->IsAnotherRunning()) {   // old copy running: kill it, take over
        kill_old_instance();
        for (int i = 0; i < 30 && single_->IsAnotherRunning(); ++i) {
            wxMilliSleep(100);
            single_ = std::make_unique<wxSingleInstanceChecker>("hard-drive-monitor-gui-" + wxGetUserId());
        }
    }
    if (from_json_.empty() && single_->IsAnotherRunning()) {
        wxMessageBox("Hard Drive Monitor is already running in the system tray.",
                     U(std::string("Hard Drive Monitor ") + GUI_VERSION));
        return false;
    }
    if (from_json_.empty()) {
        make_dirs(data_dir());
        write_file(pid_path(), std::to_string(static_cast<long>(wxGetProcessId())) + "\n");
    }
    cleanup_old_binaries();   // Windows: hdmon-gui.old.exe from a previous update
    SetExitOnFrameDelete(false);
    settings_ = g::load_gui_settings(Config());

    // Update notices (opt-in [update] auto_check) go through the tray instead of notify-send.
    update_hooks().notify = [this](const std::string& t, const std::string& b) {
        CallAfter([this, t, b] { Notify(t, b, false); });
        return std::make_pair(true, std::string("tray"));
    };

    if (wxTaskBarIcon::IsAvailable()) {
        tray_ = new TrayIcon();
        tray_->SetIcon(circle_icon("UNKNOWN"), U(g::tooltip_scanning(GUI_VERSION)));
#ifdef __WXMSW__
        wxNotificationMessage::UseTaskBarIcon(tray_);
#endif
    }
    timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ScanNow(); });
    ApplyTimer();
    if (show_ || !tray_) ShowWindow();
    ScanNow();
    if (!open_.empty()) {
        CallAfter([this] {
            if (open_ == "options") ShowOptions();
            else if (open_ == "about") ShowAbout();
            else if (open_ == "tray-menu") ShowTrayMenu();
            else if (open_ == "details" || open_ == "raid") {
                ShowWindow();
                // details need data: retry until the first scan finished
                auto* t = new wxTimer();
                t->Bind(wxEVT_TIMER, [this, t](wxTimerEvent&) {
                    if (!have_) return;
                    t->Stop();
                    if (open_ == "raid" ? frame_->SelectFirstArray() : (frame_->SelectFirst(), true)) frame_->ShowDetails();
                    t->CallAfter([t] { delete t; });
                });
                t->Start(200);
            }
        });
    }
    return true;
}

int HdmonApp::OnExit() {
    timer_.Stop();
    if (from_json_.empty() && g::parse_pid_file(read_file(pid_path())) == static_cast<long>(wxGetProcessId()))
        std::remove(pid_path().c_str());
    if (worker_.joinable()) worker_.join();
    return wxApp::OnExit();
}

void HdmonApp::ShowWindow() {
    if (!frame_) frame_ = new MainFrame();
    frame_->ShowScan(last_.results, last_.arrays, have_, scanning_, StatusLine());
    frame_->Show();
    if (frame_->IsIconized()) frame_->Iconize(false);
    frame_->Raise();
}

void HdmonApp::ShowOptions() {
    if (dialog_open_) return;
    dialog_open_ = true;
    settings_ = g::load_gui_settings(Config());
    bool was = autostart_enabled();
    int rc;
    g::GuiSettings res;
    bool run;
    {
        OptionsDialog dlg(Owner(), settings_, was);
        rc = dlg.ShowModal();
        res = dlg.Result();
        run = dlg.RunAtStart();
    }
    dialog_open_ = false;
    if (rc == OptionsDialog::ID_RESTART) { Restart(); return; }
    if (rc != wxID_OK) return;
    std::string err;
    if (run != was || run) {   // re-registering also refreshes the path if the app moved (VB behaviour)
        if (!set_autostart(run, self_exe_path(), &err))
            wxMessageBox(U("Could not change Start at login:\n" + err), "Options", wxOK | wxICON_WARNING, Owner());
    }
    if (!g::save_gui_settings(config_, res, &err))
        wxMessageBox(U("Could not save the options:\n" + err), "Options", wxOK | wxICON_WARNING, Owner());
    settings_ = res;
    ApplyTimer();
}

void HdmonApp::ShowAbout() {
    if (dialog_open_) return;
    dialog_open_ = true;
    {
        AboutDialog dlg(Owner(), g::used_smartctl(last_.results, last_.meta));
        dlg.ShowModal();
    }
    dialog_open_ = false;
}

void HdmonApp::Notify(const std::string& title, const std::string& body, bool error) {
    std::string b = body.size() > 250 ? body.substr(0, 247) + "..." : body;
#ifdef __WXMSW__
    if (tray_) { tray_->ShowBalloon(U(title), U(b), 15000, error ? wxICON_ERROR : wxICON_INFORMATION); return; }
#endif
    wxNotificationMessage n(U(title), U(b), nullptr, error ? wxICON_ERROR : wxICON_INFORMATION);
    n.Show(15);
}

void HdmonApp::ScanNow() {
    if (scanning_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    Refresh();
    json cfg = Config();
    auto from = from_json_;
    std::string db = db_;
    worker_ = std::thread([this, cfg, from, db] {
        ScanOutcome out;
        try {
            Scan sc = scan(cfg, from);
            out.results = std::move(sc.results);
            out.arrays = std::move(sc.arrays);
            out.meta = std::move(sc.meta);
            if (from.empty()) {   // like the CLI: --from-json runs are not recorded
                History h(db);
                Changes ach;
                Changes ch = h.record(out.results, jstr(out.meta, "source").value_or("live"), "", &out.arrays, &ach);
                out.alerts = build_alerts(out.results, ch);
                for (auto& a : build_array_alerts(out.arrays, out.results, ach)) out.alerts.push_back(a);
                out.alerts = filter_alerts(cfg, out.alerts);   // [alerts] min_level
                if (!out.alerts.empty() && truthy(jget(jget(cfg, "email"), "enabled")) &&
                    g::load_gui_settings(cfg).alert_on_danger) {
                    send_email(cfg, "Hard Drive Monitor", alert_text(out.alerts));
                }
                scheduled_update_check(cfg, db, true, static_cast<int64_t>(std::time(nullptr)), UpdateFlavor::Gui);
            }
        } catch (const std::exception& e) {
            out.error = e.what();
        }
        out.finished = local_now_hms().substr(11, 5);
        wxGetApp().CallAfter([out] { wxGetApp().OnScanDone(out); });
    });
}

std::string HdmonApp::StatusLine() const {
    if (scanning_) return "Scanning drives...";
    if (!last_.error.empty()) return "Last scan failed: " + replace_all(last_.error, "\n", " ");
    if (!have_) return "";
    std::string s = "Last scan " + last_.finished + "  \u00B7  " + std::to_string(last_.results.size()) + " drive(s)" +
                    (last_.arrays.empty() ? std::string() : ", " + std::to_string(last_.arrays.size()) + " RAID array(s)") +
                    "  \u00B7  worst: " + g::status_label(g::tray_status(last_.results, last_.arrays));
    std::string src = g::source_text(last_.results, last_.meta);
    if (!src.empty()) s += "  \u00B7  " + src;
    if (auto w = jstr(last_.meta, "warning")) s += "  \u00B7  " + *w;
    for (auto& [d, a] : last_.results)
        for (auto& n : d.notes)
            if (contains(n, "Needs elevated rights")) return s + "  \u00B7  some drives need admin/root rights: " + elevation_hint();
    return s;
}

void HdmonApp::Refresh() {
    if (tray_) {
        if (scanning_ && !have_) tray_->SetIcon(circle_icon("UNKNOWN"), U(g::tooltip_scanning(GUI_VERSION)));
        else if (!last_.error.empty()) tray_->SetIcon(circle_icon("UNKNOWN"), U(g::tooltip_failed(GUI_VERSION)));
        else if (have_)
            tray_->SetIcon(circle_icon(g::tray_status(last_.results, last_.arrays)),
                           U(g::tooltip_text(last_.results, last_.arrays, GUI_VERSION, current_os() == OS::Windows ? 127 : 1000)));
    }
    if (frame_) frame_->ShowScan(last_.results, last_.arrays, have_, scanning_, StatusLine());
}

void HdmonApp::OnScanDone(ScanOutcome out) {
    if (worker_.joinable()) worker_.join();
    scanning_ = false;
    bool ok = out.error.empty();
    auto alerts = out.alerts;
    last_ = std::move(out);
    if (ok) have_ = true;
    else last_.results.clear(), have_ = true;
    Refresh();
    settings_ = g::load_gui_settings(Config());
    std::vector<Alert> danger, info;
    for (auto& a : alerts) (a.severity == "OK" ? info : danger).push_back(a);   // OK = RAID rebuild complete
    if (!danger.empty() && settings_.alert_on_danger) {
        Notify("Hard Drive Monitor", alert_text(danger), true);
        auto* dlg = new AlertDialog(danger);
        dlg->Show();
        dlg->Raise();
    }
    if (!info.empty() && settings_.alert_on_danger) Notify("Hard Drive Monitor", alert_text(info), false);
}

void HdmonApp::Restart() {
    std::string exe = self_exe_path();
    single_.reset();   // let the new copy start
    wxExecute(U("\"" + exe + "\""), wxEXEC_ASYNC);
    ExitApp();
}

void HdmonApp::ExitApp() {
    timer_.Stop();
    if (tray_) {
        tray_->RemoveIcon();
        tray_->Destroy();
        tray_ = nullptr;
    }
    if (frame_) {
        frame_->Destroy();
        frame_ = nullptr;
    }
    ExitMainLoop();
}

// ------------------------------------------------------------------ entry points
#ifdef __WXMSW__
int WINAPI WinMain(HINSTANCE h, HINSTANCE prev, LPSTR, int show) {
    for (int i = 1; i < __argc; ++i) {
        std::string a = __argv[i];
        if (a == "--version") {
            // GUI subsystem: print to a parent console if there is one.
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* f = nullptr;
                freopen_s(&f, "CONOUT$", "w", stdout);
            }
            std::printf("Hard Drive Monitor (hdmon-gui) %s\n", GUI_VERSION);
            std::fflush(stdout);
            return 0;
        }
    }
    return wxEntry(h, prev, nullptr, show);
}
#else
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version") {
            std::printf("Hard Drive Monitor (hdmon-gui) %s\n", GUI_VERSION);
            return 0;
        }
        if (a == "-h" || a == "--help") {
            std::printf("usage: hdmon-gui [--show] [--from-json FILE_OR_DIR]... [--config FILE] [--db FILE]\n"
                        "                 [--open options|about|details|raid|tray-menu] [--version]\n\n"
                        "Hard Drive Monitor desktop app: tray icon in the worst drive's colour, drive list,\n"
                        "hourly background scans. The command-line tool is `hdmon`.\n");
            return 0;
        }
    }
    return wxEntry(argc, argv);
}
#endif
