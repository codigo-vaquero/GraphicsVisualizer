#include <wx/wx.h>
#include <wx/dirctrl.h>
#include <wx/splitter.h>
#include <wx/image.h>
#include <wx/dcbuffer.h>

// nanosvg — viene en los headers de wxWidgets (3.1.6+)
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

class ImagePanel : public wxPanel
{
    wxBitmap m_bitmap;
    wxString m_path;

public:
    ImagePanel(wxWindow* parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ImagePanel::OnPaint, this);
        Bind(wxEVT_SIZE,  &ImagePanel::OnSize,  this);
    }

    void LoadImage(const wxString& path)
    {
        m_path = path;
        m_bitmap = wxNullBitmap;

        wxString ext = path.AfterLast('.').Lower();

        if (ext == "svg")
            LoadSVG(path);
        else
            LoadRaster(path);

        Refresh();
    }

private:
    void LoadRaster(const wxString& path)
    {
        wxImage img(path);
        if (img.IsOk())
        {
            m_bitmap = wxBitmap(img);
            FitToPanel();
        }
    }

    void LoadSVG(const wxString& path)
    {
        wxSize sz = GetClientSize();
        int targetW = std::max(sz.x, 64);
        int targetH = std::max(sz.y, 64);

        NSVGimage* svg = nsvgParseFromFile(
            path.ToUTF8().data(), "px", 96.0f);

        if (!svg) return;

        float scaleX = (float)targetW / svg->width;
        float scaleY = (float)targetH / svg->height;
        float scale  = std::min(scaleX, scaleY);

        int rasterW = (int)(svg->width  * scale);
        int rasterH = (int)(svg->height * scale);

        if (rasterW < 1 || rasterH < 1)
        {
            nsvgDelete(svg);
            return;
        }

        NSVGrasterizer* rast = nsvgCreateRasterizer();
        std::vector<unsigned char> buf(rasterW * rasterH * 4);

        nsvgRasterize(rast, svg, 0, 0, scale,
                      buf.data(), rasterW, rasterH, rasterW * 4);

        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);

        // nanosvg produce RGBA, wxImage espera RGB + alpha separado
        wxImage img(rasterW, rasterH);
        img.InitAlpha();

        for (int i = 0; i < rasterW * rasterH; ++i)
        {
            img.SetRGB(i % rasterW, i / rasterW,
                buf[i*4+0], buf[i*4+1], buf[i*4+2]);
            img.SetAlpha(i % rasterW, i / rasterW,
                buf[i*4+3]);
        }

        m_bitmap = wxBitmap(img);
    }

    void FitToPanel()
    {
        if (!m_bitmap.IsOk()) return;
        wxSize sz = GetClientSize();
        if (sz.x < 1 || sz.y < 1) return;

        wxImage img = m_bitmap.ConvertToImage();
        double scaleX = (double)sz.x / img.GetWidth();
        double scaleY = (double)sz.y / img.GetHeight();
        double scale  = std::min(scaleX, scaleY);

        int newW = (int)(img.GetWidth()  * scale);
        int newH = (int)(img.GetHeight() * scale);
        m_bitmap = wxBitmap(img.Scale(newW, newH, wxIMAGE_QUALITY_HIGH));
    }

    void OnSize(wxSizeEvent& evt)
    {
        if (!m_path.IsEmpty())
        {
            wxString ext = m_path.AfterLast('.').Lower();
            // SVG se re-rasteriza al nuevo tamaño para máxima calidad
            if (ext == "svg")
                LoadSVG(m_path);
            else
                FitToPanel();
        }
        Refresh();
        evt.Skip();
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.Clear();

        if (!m_bitmap.IsOk())
        {
            dc.SetTextForeground(wxColour(150, 150, 150));
            dc.DrawText("Selecciona una imagen", 20, 20);
            return;
        }

        wxSize sz = GetClientSize();
        int x = (sz.x - m_bitmap.GetWidth())  / 2;
        int y = (sz.y - m_bitmap.GetHeight()) / 2;
        dc.DrawBitmap(m_bitmap, x, y, true); // true = usar alpha
    }
};

class MyFrame : public wxFrame
{
    wxGenericDirCtrl* m_dirCtrl;
    ImagePanel*       m_imgPanel;

public:
    MyFrame()
        : wxFrame(nullptr, wxID_ANY, "Image Viewer",
                  wxDefaultPosition, wxSize(900, 600))
    {
        wxInitAllImageHandlers();

        auto* splitter = new wxSplitterWindow(this, wxID_ANY,
            wxDefaultPosition, wxDefaultSize,
            wxSP_LIVE_UPDATE | wxSP_THIN_SASH);

        m_dirCtrl = new wxGenericDirCtrl(splitter, wxID_ANY,
            wxDirDialogDefaultFolderStr,
            wxDefaultPosition, wxDefaultSize,
            wxDIRCTRL_SHOW_FILTERS,
            "Imágenes (*.png;*.jpg;*.bmp;*.gif;*.svg)"
            "|*.png;*.jpg;*.bmp;*.gif;*.svg");

        m_imgPanel = new ImagePanel(splitter);

        splitter->SplitVertically(m_dirCtrl, m_imgPanel, 260);
        splitter->SetMinimumPaneSize(100);

        m_dirCtrl->Bind(wxEVT_DIRCTRL_FILEACTIVATED,
            &MyFrame::OnFileSelected, this);

        CreateStatusBar();
        SetStatusText("Listo");
    }

private:
    void OnFileSelected(wxCommandEvent&)
    {
        wxString path = m_dirCtrl->GetFilePath();
        if (path.IsEmpty()) return;

        m_imgPanel->LoadImage(path);
        SetStatusText(path);
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
