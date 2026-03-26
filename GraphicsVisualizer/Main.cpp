#include <wx/wx.h>
#include <wx/dirctrl.h>
#include <wx/splitter.h>
#include <wx/image.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/dir.h>
#include <wx/gauge.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

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
static const int BUFFER_ROWS = 1;

wxDEFINE_EVENT(wxEVT_THUMB_READY, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_LOADING_DONE, wxCommandEvent);

// ── diálogo de progreso modal ────────────────────────────────────────────────
class LoadingDialog : public wxDialog
{
    wxGauge* m_gauge;
    wxStaticText* m_label;
    int           m_total;

public:
    LoadingDialog(wxWindow* parent, int total)
        : wxDialog(parent, wxID_ANY, "Cargando imágenes",
            wxDefaultPosition, wxSize(360, 120),
            wxCAPTION | wxSTAY_ON_TOP)
        , m_total(total)
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        m_label = new wxStaticText(this, wxID_ANY,
            wxString::Format("Cargando 0 de %d...", total));
        sizer->Add(m_label, 0, wxALL | wxEXPAND, 12);

        m_gauge = new wxGauge(this, wxID_ANY, total,
            wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL | wxGA_SMOOTH);
        sizer->Add(m_gauge, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        SetSizerAndFit(sizer);
        Centre();
    }

    void Update(int done)
    {
        m_gauge->SetValue(done);
        m_label->SetLabel(
            wxString::Format("Cargando %d de %d...", done, m_total));

        if (done >= m_total)
            EndModal(wxID_OK);
    }
};

// ── estado de cada item ──────────────────────────────────────────────────────
enum class ThumbState { Pending, Loading, Ready, Failed };

struct ThumbItem
{
    wxString   path;
    wxString   name;
    wxBitmap   thumb;
    ThumbState state = ThumbState::Pending;
    bool       selected = false;
};

// ── worker thread ────────────────────────────────────────────────────────────
class ThumbLoader : public wxThread
{
    wxEvtHandler* m_sink;
    wxString           m_path;
    int                m_index;
    std::atomic<bool>& m_cancelled;

public:
    ThumbLoader(wxEvtHandler* sink, const wxString& path,
        int index, std::atomic<bool>& cancelled)
        : wxThread(wxTHREAD_DETACHED)
        , m_sink(sink), m_path(path)
        , m_index(index), m_cancelled(cancelled) {
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
        NSVGimage* svg = nsvgParseFromFile(
            path.ToUTF8().data(), "px", 96.0f);
        if (!svg) return wxImage();

        float scale = (float)size / std::max(svg->width, svg->height);
        int w = (int)(svg->width * scale);
        int h = (int)(svg->height * scale);
        if (w < 1 || h < 1) { nsvgDelete(svg); return wxImage(); }

        NSVGrasterizer* rast = nsvgCreateRasterizer();
        std::vector<unsigned char> buf(w * h * 4);
        nsvgRasterize(rast, svg, 0, 0, scale, buf.data(), w, h, w * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);

        wxImage img(w, h);
        img.InitAlpha();
        for (int i = 0; i < w * h; ++i)
        {
            img.SetRGB(i % w, i / w, buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]);
            img.SetAlpha(i % w, i / w, buf[i * 4 + 3]);
        }
        return img;
    }
};

// ── grid panel ───────────────────────────────────────────────────────────────
class GridPanel : public wxScrolledWindow
{
    std::vector<ThumbItem> m_items;
    std::queue<int>        m_pending;
    std::set<int>          m_loading;
    int                    m_activeThreads = 0;
    int                    m_cols = 1;
    int                    m_selected = -1;
    int                    m_doneCount = 0;
    int                    m_generation = 0;
    std::atomic<bool>      m_cancelled{ false };
    wxTimer                m_scrollTimer;

    LoadingDialog* m_loadDlg = nullptr;

public:
    GridPanel(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY)
        , m_scrollTimer(this)
    {
        SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        SetScrollRate(0, 8);

        Bind(wxEVT_PAINT, &GridPanel::OnPaint, this);
        Bind(wxEVT_SIZE, &GridPanel::OnSize, this);
        Bind(wxEVT_LEFT_DOWN, &GridPanel::OnClick, this);
        Bind(wxEVT_LEFT_DCLICK, &GridPanel::OnDClick, this);
        Bind(wxEVT_MOUSEWHEEL, &GridPanel::OnWheel, this);
        Bind(wxEVT_THUMB_READY, &GridPanel::OnThumbReady, this);
        Bind(wxEVT_TIMER, &GridPanel::OnScrollTimer, this,
            m_scrollTimer.GetId());

        Bind(wxEVT_SCROLLWIN_TOP, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_BOTTOM, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_LINEUP, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_LINEDOWN, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_PAGEUP, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_PAGEDOWN, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_THUMBTRACK, &GridPanel::OnScroll, this);
        Bind(wxEVT_SCROLLWIN_THUMBRELEASE, &GridPanel::OnScroll, this);
    }

    void LoadFolder(const wxString& dir)
    {
        // cancelar generación anterior
        m_cancelled = true;
        m_generation++;

        m_items.clear();
        m_selected = -1;
        m_activeThreads = 0;
        m_doneCount = 0;
        m_loading.clear();
        while (!m_pending.empty()) m_pending.pop();

        static const wxString exts[] = {
            "png","jpg","jpeg","bmp","gif","svg","tiff","tif","webp" };

        wxDir d(dir);
        if (d.IsOpened())
        {
            wxString fname;
            bool ok = d.GetFirst(&fname, wxEmptyString, wxDIR_FILES);
            while (ok)
            {
                wxString ext = fname.AfterLast('.').Lower();
                for (auto& e : exts)
                {
                    if (ext == e)
                    {
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

        if (m_items.empty())
        {
            m_cancelled = false;
            RecalcLayout();
            Refresh();
            return;
        }

        for (int i = 0; i < (int)m_items.size(); ++i)
            m_pending.push(i);

        m_cancelled = false;
        RecalcLayout();
        Scroll(0, 0);
        Refresh();

        // lanzar TODOS los hilos (no solo los visibles)
        // el diálogo bloquea hasta que todos terminen
        DispatchAll();

        // mostrar diálogo modal — regresa cuando Update() llama EndModal
        m_loadDlg = new LoadingDialog(wxGetTopLevelParent(this),
            (int)m_items.size());
        m_loadDlg->ShowModal();
        m_loadDlg->Destroy();
        m_loadDlg = nullptr;

        // ahora el grid tiene todos los thumbs — solo renderizar visibles
        Refresh();
    }

    int GetCount() const { return (int)m_items.size(); }

private:
    // lanza hilos para TODOS los pendientes (hasta MAX_THREADS a la vez;
    // los demás se despachan desde OnThumbReady)
    void DispatchAll()
    {
        while (m_activeThreads < MAX_THREADS && !m_pending.empty())
        {
            int idx = m_pending.front();
            m_pending.pop();

            if (m_items[idx].state != ThumbState::Pending) continue;

            m_items[idx].state = ThumbState::Loading;
            m_loading.insert(idx);
            m_activeThreads++;

            (new ThumbLoader(this, m_items[idx].path, idx, m_cancelled))->Run();
        }
    }

    void RecalcLayout()
    {
        wxSize sz = GetClientSize();
        if (sz.x <= 0) sz.x = 800;
        m_cols = std::max(1, sz.x / CELL_W);
        int rows = ((int)m_items.size() + m_cols - 1) / m_cols;
        SetVirtualSize(sz.x, rows * CELL_H + THUMB_PAD);
    }

    wxRect CellRect(int i) const
    {
        return { (i % m_cols) * CELL_W, (i / m_cols) * CELL_H, CELL_W, CELL_H };
    }

    void GetVisibleRange(int& first, int& last) const
    {
        int ppuX = 0, ppuY = 0;
        GetScrollPixelsPerUnit(&ppuX, &ppuY);
        int scrollY = GetScrollPos(wxVERTICAL) * (ppuY > 0 ? ppuY : 1);

        wxSize sz = GetClientSize();
        if (sz.y <= 0) sz.y = 600;
        if (sz.x <= 0) sz.x = 800;

        int firstRow = std::max(0, scrollY / CELL_H - BUFFER_ROWS);
        int lastRow = (scrollY + sz.y) / CELL_H + BUFFER_ROWS;

        first = firstRow * m_cols;
        last = std::min((int)m_items.size() - 1,
            (lastRow + 1) * m_cols - 1);
    }

    // solo se usa post-carga para el scroll lazy
    void DispatchVisible()
    {
        if (m_items.empty() || m_loadDlg != nullptr) return;

        int first, last;
        GetVisibleRange(first, last);

        std::vector<int> all;
        while (!m_pending.empty())
        {
            all.push_back(m_pending.front());
            m_pending.pop();
        }

        std::queue<int> reordered;
        std::vector<int> rest;
        for (int idx : all)
        {
            if (idx >= first && idx <= last) reordered.push(idx);
            else                             rest.push_back(idx);
        }
        for (int idx : rest) reordered.push(idx);
        m_pending = std::move(reordered);

        while (m_activeThreads < MAX_THREADS && !m_pending.empty())
        {
            int idx = m_pending.front();
            m_pending.pop();
            if (m_items[idx].state != ThumbState::Pending) continue;
            m_items[idx].state = ThumbState::Loading;
            m_loading.insert(idx);
            m_activeThreads++;
            (new ThumbLoader(this, m_items[idx].path, idx, m_cancelled))->Run();
        }
    }

    // ── handlers ─────────────────────────────────────────────────────────────

    void OnThumbReady(wxCommandEvent& evt)
    {
        int idx = evt.GetInt();
        auto* img = static_cast<wxImage*>(evt.GetClientData());

        m_activeThreads = std::max(0, m_activeThreads - 1);
        m_loading.erase(idx);

        if (idx >= 0 && idx < (int)m_items.size() &&
            m_items[idx].state == ThumbState::Loading)
        {
            if (img && img->IsOk())
            {
                m_items[idx].thumb = wxBitmap(*img);
                m_items[idx].state = ThumbState::Ready;
            }
            else
            {
                m_items[idx].state = ThumbState::Failed;
            }
            m_doneCount++;
        }
        delete img;

        // alimentar siguiente hilo (durante carga inicial o scroll lazy)
        if (!m_pending.empty())
            DispatchAll();

        // actualizar diálogo si está abierto
        if (m_loadDlg)
            m_loadDlg->Update(m_doneCount);
        else
            Refresh();
    }

    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        PrepareDC(dc);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        if (m_items.empty())
        {
            dc.SetTextForeground(
                wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
            dc.DrawText("Selecciona una carpeta", 16, 16);
            return;
        }

        int first, last;
        GetVisibleRange(first, last);

        wxColour selBg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
        wxColour selFg = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
        wxColour normFg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);

        dc.SetFont(wxFont(8, wxFONTFAMILY_DEFAULT,
            wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));

        for (int i = first; i <= last; ++i)
        {
            if (i < 0 || i >= (int)m_items.size()) continue;
            auto& item = m_items[i];
            wxRect cell = CellRect(i);

            if (item.selected)
            {
                dc.SetBrush(wxBrush(selBg));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRoundedRectangle(cell.Deflate(4), 6);
            }

            wxRect thumbArea(cell.x + THUMB_PAD, cell.y + THUMB_PAD,
                THUMB_SIZE, THUMB_SIZE);

            if (item.state == ThumbState::Ready && item.thumb.IsOk())
            {
                int bx = thumbArea.x + (THUMB_SIZE - item.thumb.GetWidth()) / 2;
                int by = thumbArea.y + (THUMB_SIZE - item.thumb.GetHeight()) / 2;
                dc.DrawBitmap(item.thumb, bx, by, true);
            }
            else if (item.state == ThumbState::Failed)
            {
                dc.SetBrush(wxBrush(wxColour(250, 235, 235)));
                dc.SetPen(wxPen(wxColour(220, 180, 180)));
                dc.DrawRoundedRectangle(thumbArea, 4);
                dc.SetTextForeground(wxColour(180, 100, 100));
                dc.DrawText("?",
                    thumbArea.x + THUMB_SIZE / 2 - 4,
                    thumbArea.y + THUMB_SIZE / 2 - 7);
            }
            else
            {
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
            while (tw > CELL_W - 8 && label.Len() > 4)
            {
                label = label.Left(label.Len() - 4) + "...";
                dc.GetTextExtent(label, &tw, &th);
            }
            dc.SetTextForeground(item.selected ? selFg : normFg);
            dc.DrawText(label,
                cell.x + (CELL_W - tw) / 2,
                cell.y + THUMB_PAD * 2 + THUMB_SIZE);
        }
    }

    void OnSize(wxSizeEvent& evt)
    {
        RecalcLayout();
        Refresh();
        evt.Skip();
    }

    void OnScroll(wxScrollWinEvent& evt)
    {
        evt.Skip();
        m_scrollTimer.StartOnce(80);
    }

    void OnWheel(wxMouseEvent& evt)
    {
        evt.Skip();
        m_scrollTimer.StartOnce(80);
    }

    void OnScrollTimer(wxTimerEvent&)
    {
        DispatchVisible();
    }

    void OnClick(wxMouseEvent& evt)
    {
        wxClientDC dc(this);
        PrepareDC(dc);
        wxPoint pt = evt.GetLogicalPosition(dc);

        for (auto& item : m_items) item.selected = false;
        m_selected = -1;

        for (int i = 0; i < (int)m_items.size(); ++i)
        {
            if (CellRect(i).Contains(pt))
            {
                m_items[i].selected = true;
                m_selected = i;
                break;
            }
        }
        Refresh();
    }

    void OnDClick(wxMouseEvent& evt)
    {
        OnClick(evt);
        if (m_selected >= 0)
            wxLaunchDefaultApplication(m_items[m_selected].path);
    }
};

// ── frame ────────────────────────────────────────────────────────────────────
class MyFrame : public wxFrame
{
    wxGenericDirCtrl* m_dirCtrl;
    GridPanel* m_grid;

public:
    MyFrame()
        : wxFrame(nullptr, wxID_ANY, "Image Grid Viewer",
            wxDefaultPosition, wxSize(1000, 650))
    {
        wxInitAllImageHandlers();

        auto* splitter = new wxSplitterWindow(this, wxID_ANY,
            wxDefaultPosition, wxDefaultSize,
            wxSP_LIVE_UPDATE | wxSP_THIN_SASH);

        m_dirCtrl = new wxGenericDirCtrl(splitter, wxID_ANY,
            wxDirDialogDefaultFolderStr,
            wxDefaultPosition, wxDefaultSize,
            wxDIRCTRL_DIR_ONLY);

        m_grid = new GridPanel(splitter);

        splitter->SplitVertically(m_dirCtrl, m_grid, 240);
        splitter->SetMinimumPaneSize(120);

        m_dirCtrl->Bind(wxEVT_DIRCTRL_SELECTIONCHANGED,
            &MyFrame::OnDirSelected, this);

        CreateStatusBar();
        SetStatusText("Selecciona una carpeta");
    }

private:
    void OnDirSelected(wxCommandEvent&)
    {
        wxString dir = m_dirCtrl->GetPath();
        if (dir.IsEmpty()) return;

        m_grid->LoadFolder(dir);

        int n = m_grid->GetCount();
        SetStatusText(wxString::Format("%s  —  %d imagen%s",
            dir, n, n == 1 ? "" : "es"));
    }
};

class MyApp : public wxApp
{
public:
    bool OnInit() override
    {
        auto* frame = new MyFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(MyApp);