#pragma comment(lib, "d2d1.lib")

// C++ ライブラリ
#include <cassert>
#include <functional>
#include <sstream>

// Windows
#include <Windows.h>
#include <d2d1.h>

struct InputFunction {
    // 評価する関数
    std::function<double(double x)> func;
    // x 軸の左端
    double startX;
    // x 軸の右端
    double endX;
    // y 軸の上
    double startY;
    // y 軸の下
    double endY;
};

// グラフを表示する関数をつくる
InputFunction CreateInputFunction()
{
    return InputFunction{
        [](double x) { return sin(x); },
        0.0, 6.0,
        -1.5, 1.5
    };
}

// 失敗したらそのエラーコードで return するマクロ
// デバッグビルドではブレークする
#ifdef _DEBUG
#define TRYRET(expr) do { HRESULT hr = (expr); if (FAILED(hr)) { DebugBreak(); return hr; } } while(0)
#else
#define TRYRET(expr) do { HRESULT hr = (expr); if (FAILED(hr)) return hr; } while (0)
#endif

// デバッグコンソールへの書き込みを書きやすくする
void WriteToDebugConsole(std::function<void(std::wostream&)> stringFactory)
{
    std::wostringstream os;
    stringFactory(os);
    std::wstring s = os.str();
    OutputDebugStringW(s.c_str());
}

// エラーをデバッグコンソールに表示
void ReportWin32Error()
{
    DWORD error = GetLastError();
    WriteToDebugConsole([error](std::wostream& s) {
        s << L"Win32Error "
            << std::hex << error
            << std::endl;
    });
}

// nullptr でなければ Release
template<class Interface>
void SafeRelease(Interface **obj)
{
    if (*obj != nullptr) {
        (*obj)->Release();
        *obj = nullptr;
    }
}

class App {
public:
    App(InputFunction inputFunction);
    ~App();

    // Direct2D の初期化と画面表示
    HRESULT Initialize(HINSTANCE hInstance);

    // メインループ
    int Run();

private:
    // WndProc を受け取って、インスタンスの WndProcCore に投げる
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    // ウィンドウイベント処理
    LRESULT WndProcCore(UINT message, WPARAM wParam, LPARAM lParam);

    // Direct2D 描画コンテキストの初期化
    HRESULT CreateDeviceResources();

    void OnResize(UINT32 width, UINT32 height);
    HRESULT OnRender();

    // 以下フィールド
    InputFunction m_inputFunction;

    HWND m_hwnd;
    ID2D1Factory* m_pDirect2dFactory;
    ID2D1HwndRenderTarget* m_pRenderTarget;

    // グラフの線の色（赤）
    ID2D1SolidColorBrush* m_pGraphLineBrush;
};

App::App(InputFunction inputFunction)
    : m_inputFunction(inputFunction),
    m_hwnd(nullptr),
    m_pDirect2dFactory(nullptr),
    m_pRenderTarget(nullptr),
    m_pGraphLineBrush(nullptr)
{
}

App::~App()
{
    SafeRelease(&m_pDirect2dFactory);
    SafeRelease(&m_pRenderTarget);
    SafeRelease(&m_pGraphLineBrush);
}

HRESULT App::Initialize(HINSTANCE hInstance)
{
    // Direct2D 初期化
    TRYRET(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pDirect2dFactory));

    // ウィンドウ作成
    WNDCLASSEX wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = sizeof(LONG_PTR);
    wcex.hInstance = hInstance;
    wcex.hIcon = NULL;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = TEXT("GraphViewer");
    wcex.hIconSm = NULL;

    ATOM atom = RegisterClassEx(&wcex);
    if (atom == 0) {
        ReportWin32Error();
#ifdef _DEBUG
        DebugBreak();
#endif
        return E_FAIL;
    }

    FLOAT dpiX, dpiY;
    m_pDirect2dFactory->GetDesktopDpi(&dpiX, &dpiY);

    m_hwnd = CreateWindow(
        (LPCTSTR)atom,
        TEXT("GraphViewer"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(640.0f * dpiX / 96.0f),
        static_cast<int>(480.0f * dpiY / 96.0f),
        NULL,
        NULL,
        hInstance,
        this
    );

    if (m_hwnd == NULL) {
        ReportWin32Error();
#ifdef _DEBUG
        DebugBreak();
#endif
        return E_FAIL;
    }

    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    UpdateWindow(m_hwnd);

    return S_OK;
}

int App::Run()
{
    MSG msg;

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CREATE) {
        LPCREATESTRUCT pcs = (LPCREATESTRUCT)lParam;
        App *pApp = (App*)pcs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pApp));
        return 1;
    } else {
        LONG_PTR userData = (LONG_PTR)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        App *pApp = reinterpret_cast<App*>(userData);
        if (pApp != nullptr) {
            assert(hwnd == pApp->m_hwnd);
            return pApp->WndProcCore(message, wParam, lParam);
        }
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

LRESULT App::WndProcCore(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DISPLAYCHANGE:
        // 画面書き換えを要求
        InvalidateRect(m_hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
    {
        HRESULT renderResult = OnRender();
        if (SUCCEEDED(renderResult)) {
            ValidateRect(m_hwnd, NULL);
        } else {
            WriteToDebugConsole([renderResult](std::wostream& s) {
                s << L"描画エラー "
                    << std::hex << renderResult
                    << std::endl;
            });
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 1;
    }

    return DefWindowProc(m_hwnd, message, wParam, lParam);
}

HRESULT App::CreateDeviceResources()
{
    // RenderTarget 作成
    if (m_pRenderTarget == nullptr) {
        RECT clientRect;
        GetClientRect(m_hwnd, &clientRect);

        TRYRET(m_pDirect2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(), // すべてデフォルト
            D2D1::HwndRenderTargetProperties(
                m_hwnd,
                D2D1::SizeU(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top)
            ),
            &m_pRenderTarget
        ));
    }

    // ブラシ作成
    if (m_pGraphLineBrush == nullptr) {
        TRYRET(m_pRenderTarget->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::Red),
            &m_pGraphLineBrush
        ));
    }

    return S_OK;
}

void App::OnResize(UINT32 width, UINT32 height)
{
    if (m_pRenderTarget != nullptr) {
        // Direct2D の描画サイズ変更
        m_pRenderTarget->Resize(D2D1::SizeU(width, height));
    }
}

// 与えられた画面上の x 座標に対応する点の座標を求める
D2D1_POINT_2F ComputePoint(const InputFunction& inputFunction, D2D1_SIZE_F size, int x)
{
    double argX = ((double)x / size.width)
        * (inputFunction.endX - inputFunction.startX)
        + inputFunction.startX;

    double value = inputFunction.func(argX);

    double y = size.height
        - size.height * (
            (value - inputFunction.startY)
            / (inputFunction.endY - inputFunction.startY)
        );

    return D2D1::Point2F(static_cast<FLOAT>(x), static_cast<FLOAT>(y));
}

HRESULT App::OnRender()
{
    TRYRET(CreateDeviceResources());

    // 描画開始
    m_pRenderTarget->BeginDraw();
    m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    m_pRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::White));

    D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();
    D2D1_POINT_2F prevPoint = ComputePoint(m_inputFunction, rtSize, 0);

    int maxX = static_cast<int>(ceil(rtSize.width));

    // 1px ごとに計算して線を引く
    for (int x = 1; x <= maxX; x++) {
        D2D1_POINT_2F p = ComputePoint(m_inputFunction, rtSize, x);
        m_pRenderTarget->DrawLine(prevPoint, p, m_pGraphLineBrush, 2.0, NULL);
        prevPoint = p;
    }

    HRESULT hr = m_pRenderTarget->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET) {
        hr = S_OK();
        SafeRelease(&m_pRenderTarget);
        SafeRelease(&m_pGraphLineBrush);
    }

    return hr;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    int exitCode = 1;

    if (SUCCEEDED(CoInitialize(NULL))) {
        App app(CreateInputFunction());

        if (SUCCEEDED(app.Initialize(hInstance))) {
            exitCode = app.Run();
        }

        CoUninitialize();
    }

    return exitCode;
}
