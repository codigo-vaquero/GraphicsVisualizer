#include <wx/wx.h>
#include <wx/dirctrl.h>
#include <wx/splitter.h>
#include <wx/image.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/dir.h>
#include <wx/gauge.h>
#include <wx/menu.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>
#include "RepoHandling.h"

#include <vector>
#include <queue>
#include <set>
#include <algorithm>
#include <atomic>

static const int THUMB_SIZE = 120;
static const int THUMB_PAD = 12;
static const int LABEL_H = 18;
static const int CELL_W = THUMB_SIZE + THUMB_PAD * 2;
static const int CELL_H = THUMB_SIZE + LABEL_H + THUMB_PAD * 3;
static const int MAX_THREADS = 4;
static const int BUFFER_ROWS = 2;

wxDEFINE_EVENT(wxEVT_THUMB_READY, wxCommandEvent);

// ── LoadingDialog ─────────────────────────────────────────────
class LoadingDialog : public wxDialog{
    wxGauge* m_gauge;
    wxStaticText* m_label;
    int m_total;
    int m_lastUpdate = 0;
    static const int UPDATE_STEP = 8;

public:
    LoadingDialog(wxWindow* parent, int total)
        : wxDialog(parent, wxID_ANY, "Cargando imágenes",
            wxDefaultPosition, wxSize(380, 130),
            wxCAPTION | wxSTAY_ON_TOP)
        , m_total(total){

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_label = new wxStaticText(this, wxID_ANY,
            wxString::Format("Cargando 0 de %d...", total));
        sizer->Add(m_label, 0, wxALL | wxEXPAND, 15);

        m_gauge = new wxGauge(this, wxID_ANY, total,
            wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL | wxGA_SMOOTH);
        sizer->Add(m_gauge, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 15);

        SetSizerAndFit(sizer);
        Centre();
        m_gauge->SetValue(0);
    }

    void Update(int done){
        if(done - m_lastUpdate < UPDATE_STEP && done < m_total)
            return;

        m_lastUpdate = done;

        m_gauge->SetValue(done);
        m_label->SetLabel(wxString::Format("Cargando %d de %d...", done, m_total));
        m_gauge->Update();
        m_label->Update();

        if (done >= m_total)
            EndModal(wxID_OK);
    }
};

// ── ThumbItem y ThumbLoader ───────────────────────
enum class ThumbState { Pending, Loading, Ready, Failed };

struct ThumbItem
{
    wxString path;
    wxString name;
    wxBitmap thumb;
    ThumbState state = ThumbState::Pending;
    bool selected = false;
};

class ThumbLoader : public wxThread
{
    wxEvtHandler* m_sink;
    wxString m_path;
    int m_index;
    std::atomic<bool>& m_cancelled;

public:
    ThumbLoader(wxEvtHandler* sink, const wxString& path, int index,
        std::atomic<bool>& cancelled)
        : wxThread(wxTHREAD_DETACHED)
        , m_sink(sink), m_path(path), m_index(index), m_cancelled(cancelled) {
    }

protected:
    ExitCode Entry() override
    {
        wxImage img;
        if (!m_cancelled)
        {
            wxString ext = m_path.AfterLast('.').Lower();
            if (ext == "svg")
                img = RasterizeSVG(m_path, THUMB_SIZE);
            else
                img.LoadFile(m_path);

            if (img.IsOk() && !m_cancelled)
            {
                double sx = (double)THUMB_SIZE / img.GetWidth();
                double sy = (double)THUMB_SIZE / img.GetHeight();
                double s = std::min(sx, sy);
                img = img.Scale((int)(img.GetWidth() * s),
                    (int)(img.GetHeight() * s),
                    wxIMAGE_QUALITY_HIGH);
            }
        }

        if (!m_cancelled)
        {
            auto* evt = new wxCommandEvent(wxEVT_THUMB_READY);
            evt->SetInt(m_index);
            evt->SetClientData(img.IsOk() ? new wxImage(img) : nullptr);
            wxQueueEvent(m_sink, evt);
        }
        return nullptr;
    }

    static wxImage RasterizeSVG(const wxString& path, int size)
    {
        NSVGimage* svg = nsvgParseFromFile(path.ToUTF8().data(), "px", 96.0f);
        if (!svg) return wxImage();

        float scale = (float)size / std::max(svg->width, svg->height);
        int w = (int)(svg->width * scale);
        int h = (int)(svg->height * scale);
        if (w < 1 || h < 1) {
            nsvgDelete(svg);
            return wxImage();
        }

        NSVGrasterizer* rast = nsvgCreateRasterizer();
        std::vector<unsigned char> buf(w * h * 4);
        nsvgRasterize(rast, svg, 0, 0, scale, buf.data(), w, h, w * 4);

        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);

        wxImage img(w, h);
        img.InitAlpha();
        for (int i = 0; i < w * h; ++i)
        {
            int x = i % w;
            int y = i / w;
            img.SetRGB(x, y, buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]);
            img.SetAlpha(x, y, buf[i * 4 + 3]);
        }
        return img;
    }
};

// ── GridPanel keyboard navigation──────────────────────────────────────────
class GridPanel : public wxScrolledWindow{
    std::vector<ThumbItem> m_items;
    std::queue<int> m_pending;
    std::set<int> m_loading;
    int m_activeThreads = 0;
    int m_cols = 1;
    int m_selected = -1;
    int m_doneCount = 0;
    std::atomic<bool> m_cancelled{ false };
    wxTimer m_scrollTimer;

    LoadingDialog* m_loadDlg = nullptr;

public:
    GridPanel(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxHSCROLL | wxVSCROLL | wxWANTS_CHARS | wxTAB_TRAVERSAL), m_scrollTimer(this) {

        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        SetScrollRate(0, 8);
        SetFocus();

        Bind(wxEVT_PAINT, &GridPanel::OnPaint, this);
        Bind(wxEVT_SIZE, &GridPanel::OnSize, this);
        Bind(wxEVT_LEFT_DOWN, &GridPanel::OnMouseClick, this);
        Bind(wxEVT_LEFT_DCLICK, &GridPanel::OnDClick, this);
        Bind(wxEVT_MOUSEWHEEL, &GridPanel::OnWheel, this);
        Bind(wxEVT_THUMB_READY, &GridPanel::OnThumbReady, this);
        Bind(wxEVT_TIMER, &GridPanel::OnScrollTimer, this, m_scrollTimer.GetId());
        Bind(wxEVT_SCROLLWIN_THUMBTRACK, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_THUMBRELEASE, &GridPanel::OnScroll, this);

        Bind(wxEVT_CHAR_HOOK, &GridPanel::OnCharHook, this);
        Bind(wxEVT_KEY_DOWN, &GridPanel::OnKeyDown, this);
    }

    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return true; }

    void LoadFolder(const wxString& dir){
        m_cancelled = true;
        m_items.clear();
        m_selected = -1;
        m_activeThreads = 0;
        m_doneCount = 0;
        m_loading.clear();
        while (!m_pending.empty()) m_pending.pop();

        static const wxString exts[] = { "png","jpg","jpeg","bmp","gif","svg","tiff","tif","webp" };

        wxDir d(dir);
        if(d.IsOpened()){
            wxString fname;
            bool ok = d.GetFirst(&fname, wxEmptyString, wxDIR_FILES);
            
            while(ok){
                wxString ext = fname.AfterLast('.').Lower();
                for(auto& e : exts){
                    if(ext == e){
                        ThumbItem item;
                        item.path = dir + wxFILE_SEP_PATH + fname;
                        item.name = fname;
                        m_items.push_back(item);
                        break;
                    }
                }

                ok = d.GetNext(&fname);
            }
        }

        m_cancelled = false;

        if(m_items.empty()){
            RecalcLayout();
            Refresh();
            return;
        }

        for(int i = 0; i < (int)m_items.size(); ++i)
            m_pending.push(i);

        RecalcLayout();
        Scroll(0, 0);
        Refresh();

        DispatchAll();

        m_loadDlg = new LoadingDialog(wxGetTopLevelParent(this), (int)m_items.size());
        m_loadDlg->ShowModal();
        m_loadDlg->Destroy();
        m_loadDlg = nullptr;

        Refresh();

        if (!m_items.empty())
            MoveSelection(0);
    }

    int GetCount() const { return (int)m_items.size(); }   

private:
    void DispatchAll(){
        while(m_activeThreads < MAX_THREADS && !m_pending.empty()){
            int idx = m_pending.front();
            m_pending.pop();
            if(m_items[idx].state != ThumbState::Pending) continue;

            m_items[idx].state = ThumbState::Loading;
            m_loading.insert(idx);
            m_activeThreads++;

            (new ThumbLoader(this, m_items[idx].path, idx, m_cancelled))->Run();
        }
    }

    void RecalcLayout(){
        wxSize sz = GetClientSize();
        if (sz.x <= 0) sz.x = 800;
        m_cols = std::max(1, sz.x / CELL_W);
        int rows = ((int)m_items.size() + m_cols - 1) / m_cols;
        SetVirtualSize(sz.x, rows * CELL_H + THUMB_PAD);
    }

    wxRect CellRect(int i) const{
        return { (i % m_cols) * CELL_W, (i / m_cols) * CELL_H, CELL_W, CELL_H };
    }

    void GetVisibleRange(int& first, int& last) const{
        int ppuX = 0, ppuY = 0;
        GetScrollPixelsPerUnit(&ppuX, &ppuY);
        int scrollY = GetScrollPos(wxVERTICAL) * (ppuY > 0 ? ppuY : 1);
        wxSize sz = GetClientSize();
        if (sz.y <= 0) sz.y = 600;

        int firstRow = std::max(0, scrollY / CELL_H - BUFFER_ROWS);
        int lastRow = (scrollY + sz.y) / CELL_H + BUFFER_ROWS;

        first = firstRow * m_cols;
        last = std::min((int)m_items.size() - 1, (lastRow + 1) * m_cols - 1);
    }

    void OnThumbReady(wxCommandEvent& evt){
        int idx = evt.GetInt();
        auto* img = static_cast<wxImage*>(evt.GetClientData());

        m_activeThreads = std::max(0, m_activeThreads - 1);
        m_loading.erase(idx);

        bool wasReady = false;
        if (idx >= 0 && idx < (int)m_items.size() &&
            m_items[idx].state == ThumbState::Loading){

            if(img && img->IsOk()){
                m_items[idx].thumb = wxBitmap(*img);
                m_items[idx].state = ThumbState::Ready;
            }else{
                m_items[idx].state = ThumbState::Failed;
            }

            m_doneCount++;
            wasReady = true;
        }

        delete img;

        if(!m_pending.empty())
            DispatchAll();

        if(m_loadDlg && wasReady)
            m_loadDlg->Update(m_doneCount);
        else if (!m_loadDlg)
            Refresh();
    }

    // ==================== GRID NAVIGATION ====================
    void OnCharHook(wxKeyEvent& evt){
        int key = evt.GetKeyCode();

        switch (key){
        case WXK_UP:
            MoveSelection(-m_cols);
            evt.Skip(false);
            return;
        case WXK_DOWN:
            MoveSelection(m_cols);
            evt.Skip(false);
            return;
        case WXK_LEFT:
            MoveSelection(-1);
            evt.Skip(false);
            return;
        case WXK_RIGHT:
            MoveSelection(1);
            evt.Skip(false);
            return;
        case WXK_PAGEUP:
            MoveSelection(-m_cols * 4);
            evt.Skip(false);
            return;
        case WXK_PAGEDOWN:
            MoveSelection(m_cols * 4);
            evt.Skip(false);
            return;
        case WXK_HOME:
            MoveSelection(-999999);
            evt.Skip(false);
            return;
        case WXK_END:
            MoveSelection(999999);
            evt.Skip(false);
            return;
        }

        evt.Skip();
    }

    void OnKeyDown(wxKeyEvent& evt){
        OnCharHook(evt);
    }

    void OnPaint(wxPaintEvent&){
        wxPaintDC dc(this);
        PrepareDC(dc);
        dc.Clear();

        if(m_items.empty()){
            dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
            dc.DrawText("Select a folder...", 16, 16);
            return;
        }

        int first, last;
        GetVisibleRange(first, last);

        wxColour selBg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
        wxColour selFg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
        wxColour normFg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);

        dc.SetFont(wxFont(8, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));

        for(int i = first; i <= last; ++i){
            if(i < 0 || i >= (int)m_items.size()) continue;
            auto& item = m_items[i];
            wxRect cell = CellRect(i);

            if(item.selected){
                dc.SetBrush(wxBrush(selBg));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRoundedRectangle(cell.Deflate(4), 6);
            }

            wxRect thumbArea(cell.x + THUMB_PAD, cell.y + THUMB_PAD, THUMB_SIZE, THUMB_SIZE);

            if(item.state == ThumbState::Ready && item.thumb.IsOk()){
                int bx = thumbArea.x + (THUMB_SIZE - item.thumb.GetWidth()) / 2;
                int by = thumbArea.y + (THUMB_SIZE - item.thumb.GetHeight()) / 2;
                dc.DrawBitmap(item.thumb, bx, by, true);
            }else if(item.state == ThumbState::Failed){
                dc.SetBrush(wxBrush(wxColour(250, 235, 235)));
                dc.SetPen(wxPen(wxColour(220, 180, 180)));
                dc.DrawRoundedRectangle(thumbArea, 4);
                dc.SetTextForeground(wxColour(180, 100, 100));
                dc.DrawText("?", thumbArea.x + THUMB_SIZE / 2 - 4, thumbArea.y + THUMB_SIZE / 2 - 7);
            }else{
                dc.SetBrush(wxBrush(wxColour(245, 245, 245)));
                dc.SetPen(wxPen(wxColour(210, 210, 210)));
                dc.DrawRoundedRectangle(thumbArea, 4);

                dc.SetPen(wxPen(wxColour(160, 160, 220), 2));
                int cx = thumbArea.x + THUMB_SIZE / 2;
                int cy = thumbArea.y + THUMB_SIZE / 2;
                dc.DrawArc(cx, cy - 14, cx + 14, cy, cx, cy);
            }

            wxString label = item.name;
            wxCoord tw, th;
            dc.GetTextExtent(label, &tw, &th);
            
            while(tw > CELL_W - 8 && label.Len() > 4){
                label = label.Left(label.Len() - 4) + "...";
                dc.GetTextExtent(label, &tw, &th);
            }

            dc.SetTextForeground(item.selected ? selFg : normFg);
            dc.DrawText(label, cell.x + (CELL_W - tw) / 2,
                cell.y + THUMB_PAD * 2 + THUMB_SIZE);
        }
    }

    void OnSize(wxSizeEvent& evt){
        RecalcLayout();
        Refresh();
        evt.Skip();
    }

    void OnScroll(wxScrollWinEvent& evt) { evt.Skip(); m_scrollTimer.StartOnce(80); }
    void OnWheel(wxMouseEvent& evt) { evt.Skip(); m_scrollTimer.StartOnce(80); }
    void OnScrollTimer(wxTimerEvent&) {}

    void OnMouseClick(wxMouseEvent& evt){
        SetFocus();
        wxClientDC dc(this);
        PrepareDC(dc);
        wxPoint pt = evt.GetLogicalPosition(dc);

        for (auto& item : m_items) item.selected = false;
        m_selected = -1;

        for(int i = 0; i < (int)m_items.size(); ++i){
            if(CellRect(i).Contains(pt)){
                m_items[i].selected = true;
                m_selected = i;
                break;
            }
        }

        Refresh();
        evt.Skip();
    }

    void OnDClick(wxMouseEvent& evt){
        OnMouseClick(evt);
        if(m_selected >= 0)
            wxLaunchDefaultApplication(m_items[m_selected].path);
    }

    void MoveSelection(int delta){
        if(m_items.empty()) return;

        int old = m_selected;
        if(m_selected < 0)
            m_selected = 0;
        else
            m_selected += delta;

        if(m_selected < 0) m_selected = 0;
        if(m_selected >= (int)m_items.size())
            m_selected = (int)m_items.size() - 1;

        if(m_selected == old) return;

        for(auto& item : m_items) item.selected = false;
        m_items[m_selected].selected = true;

        EnsureVisible(m_selected);
        Refresh();
    }

    void EnsureVisible(int index){
        if (index < 0 || index >= (int)m_items.size()) return;

        int row = index / m_cols;
        int yTop = row * CELL_H;
        int yBottom = yTop + CELL_H;

        int ppuX, ppuY;
        GetScrollPixelsPerUnit(&ppuX, &ppuY);
        int scrollY = GetScrollPos(wxVERTICAL) * (ppuY > 0 ? ppuY : 1);
        wxSize client = GetClientSize();

        if(yTop < scrollY)
            Scroll(0, yTop / (ppuY > 0 ? ppuY : 1));
        else if(yBottom > scrollY + client.y){
            int target = (yBottom - client.y + THUMB_PAD) / (ppuY > 0 ? ppuY : 1);
            Scroll(0, target);
        }
    }
};

// ── MyFrame ───────────────────────────────
class MyFrame : public wxFrame{
    wxGenericDirCtrl* m_dirCtrl;
    GridPanel* m_grid;

public:
    MyFrame(): wxFrame(nullptr, wxID_ANY, "Image Viewer", wxDefaultPosition, wxSize(1000, 650)){
        wxInitAllImageHandlers();

        auto* splitter = new wxSplitterWindow(this, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_THIN_SASH);

        m_dirCtrl = new wxGenericDirCtrl(splitter, wxID_ANY,
            wxDirDialogDefaultFolderStr, wxDefaultPosition, wxDefaultSize, wxDIRCTRL_DIR_ONLY);

        m_grid = new GridPanel(splitter);

        splitter->SplitVertically(m_dirCtrl, m_grid, 240);
        splitter->SetMinimumPaneSize(120);

        m_dirCtrl->Bind(wxEVT_DIRCTRL_SELECTIONCHANGED, &MyFrame::OnDirSelected, this);
        Bind(wxEVT_CHAR_HOOK, &MyFrame::OnCharHookFrame, this);

        CreateStatusBar();
        SetStatusText("Select a folder from the folder tree...");

        wxMenu* fileMenu = new wxMenu;
        fileMenu->Append(wxID_NEW, "New\tCtrl+N");
        fileMenu->Append(wxID_EXIT, "Exit\tCtrl+Q");

        wxMenuBar* menuBar = new wxMenuBar;
        menuBar->Append(fileMenu, "File");

        SetMenuBar(menuBar);

        Bind(wxEVT_MENU, [this](wxCommandEvent& evt){
            if(evt.GetId() == wxID_EXIT)
                Close();
            else if(evt.GetId() == wxID_NEW)
                saveRepo();
		});
    }

private:
    void OnDirSelected(wxCommandEvent&){
        wxString dir = m_dirCtrl->GetPath();
        if(dir.IsEmpty()) return;

        m_grid->LoadFolder(dir);

        int n = m_grid->GetCount();
        SetStatusText(wxString::Format("%s — %d Image%s", dir, n, n == 1 ? "" : "s"));

        CallAfter([this](){
            if(m_grid){
                m_grid->SetFocus();
                m_grid->SetFocusFromKbd();
            }
        });
    }

    void OnCharHookFrame(wxKeyEvent& evt){
        if(!m_grid || m_grid->GetCount() == 0){
            evt.Skip();
            return;
        }

        int key = evt.GetKeyCode();
        if(key == WXK_UP || key == WXK_DOWN || key == WXK_LEFT || key == WXK_RIGHT ||
            key == WXK_PAGEUP || key == WXK_PAGEDOWN || key == WXK_HOME || key == WXK_END){
            m_grid->SetFocus();
            wxPostEvent(m_grid, evt);

            return;
        }

        evt.Skip();
    }

    void saveRepo() {
        RepoHandling rHandler;
		rHandler.createFile();        		
    }
};

// ── App ─────────────────────────────────────────────────────────────────────
class MyApp : public wxApp{
public:
    bool OnInit() override{
        auto* frame = new MyFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(MyApp);