#include "MainWindow.hpp"
#include "Theme.hpp"
#include "shaderfmt/Formatter.hpp"
#include "shaderfmt/Token.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QPixmap>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::cerr << "  [FAIL] " << what << "\n";
        g_failures++;
    }
}

void check(bool cond, const std::string& what) {
    check(cond, what.c_str());
}

// Formatting runs off the GUI thread now (roadmap §7), so anything that
// triggers it (Format button, style panel changes, format-on-the-fly)
// must be followed by this before inspecting editor text. Connecting the
// signal and entering the event loop happen back-to-back on the GUI
// thread with no intervening event processing, so this can never miss
// the signal even if the background computation already finished by the
// time we get here - the queued finished() notification can't have been
// delivered yet.
void waitForFormat(MainWindow& window, int timeoutMs = 3000) {
    QEventLoop loop;
    QObject::connect(&window, &MainWindow::formatSettledForTesting, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
}

// Drives MainWindow through the same code paths a user's mouse/keyboard
// would (language detection, Format action, undo, clipboard, format-on-
// the-fly) without needing a human at a display. Run with
// QT_QPA_PLATFORM=offscreen so it also works in a headless build
// environment. This is what makes ROADMAP §3's items build-and-execution
// verified rather than merely "compiles" (principle #14).
int runSelfTest(QApplication& app) {
    // StylePanel persists preferences via QSettings (roadmap §4). Redirect
    // its .ini file to a throwaway temp dir so this run never reads or
    // clobbers the real user's ShaderFmt settings, and so the "settings
    // survive across a new MainWindow" check below starts from a known
    // empty state instead of whatever the last run on this machine left.
    QTemporaryDir settingsDir;
    check(settingsDir.isValid(), "temp dir for isolated QSettings is valid");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    shaderfmt_app::applyTheme(app, shaderfmt_app::ThemeMode::Dark);

    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    check(window.isDarkThemeForTesting(), "MainWindow defaults to the dark theme on first run");

    // --- Auto-detection updates the active lexer language on paste ---
    window.editorForTesting()->setText(QString::fromUtf8(
        "cbuffer PerFrame : register(b0) { float4x4 mvp; };\n"
        "float4 PSMain() : SV_Target { return float4(1,1,1,1); }\n"));
    QCoreApplication::processEvents();
    check(window.currentLanguageForTesting() == shaderfmt::Language::HLSL,
          "auto-detect switches lexer to HLSL for cbuffer/SV_Target source");

    // --- MSL is selectable from the toolbar combo and formats through
    // the same async path as every other language (roadmap §2) ---
    window.editorForTesting()->setText(QString::fromUtf8(
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 fs_main(float4 in [[stage_in]]){return in;}\n"));
    QCoreApplication::processEvents();
    check(window.currentLanguageForTesting() == shaderfmt::Language::MSL,
          "auto-detect switches lexer to MSL for metal_stdlib/[[stage_in]] source");
    window.triggerFormatForTesting();
    waitForFormat(window);
    check(window.editorForTesting()->text().contains(QStringLiteral("fragment float4 fs_main")),
          "MSL source formats correctly through the UI's Format action");

    // --- ShaderLab is selectable too, and its embedded CGPROGRAM block
    // (real HLSL, parsed for real - roadmap §2) formats through the UI ---
    window.editorForTesting()->setText(QString::fromUtf8(
        "Shader \"Custom/X\" { SubShader { Pass { CGPROGRAM\n"
        "float4 frag() : SV_Target { return float4(1,1,1,1); }\n"
        "ENDCG } } }\n"));
    QCoreApplication::processEvents();
    check(window.currentLanguageForTesting() == shaderfmt::Language::ShaderLab,
          "auto-detect switches lexer to ShaderLab for SubShader/CGPROGRAM source");
    window.triggerFormatForTesting();
    waitForFormat(window);
    check(window.editorForTesting()->text().contains(QStringLiteral("SV_Target")),
          "ShaderLab source (with embedded HLSL) formats correctly through the UI's Format action");

    // --- GLSL per-version qualifier lint (roadmap §2): advisory-only
    // squiggle indicator, distinct from lex errors, only active for GLSL ---
    window.setLanguageForTesting(shaderfmt::Language::GLSL);
    window.editorForTesting()->setText(QString::fromUtf8(
        "#version 330 core\nattribute vec3 aPos;\nvoid main(){}\n"));
    QCoreApplication::processEvents();
    check(window.glslVersionWarningIndicatorCountForTesting() == 1,
          "GLSL version lint flags 'attribute' under #version 330 through the live editor");
    window.editorForTesting()->setText(QString::fromUtf8(
        "#version 120\nattribute vec3 aPos;\nvoid main(){}\n"));
    QCoreApplication::processEvents();
    check(window.glslVersionWarningIndicatorCountForTesting() == 0,
          "GLSL version lint stays silent when the qualifier matches the declared version");
    window.setLanguageForTesting(shaderfmt::Language::HLSL);
    window.editorForTesting()->setText(QString::fromUtf8(
        "#version 330 core\nattribute vec3 aPos;\nvoid main(){}\n"));
    QCoreApplication::processEvents();
    check(window.glslVersionWarningIndicatorCountForTesting() == 0,
          "GLSL version lint never runs when the active language isn't GLSL");

    // --- Viewport: live GLSL/Shadertoy render preview. compilePendingShader()
    // runs inside paintGL() (a real GL call needs a current context) -
    // QWidget::repaint() is documented synchronous (unlike update(), which
    // just schedules an async repaint the offscreen QPA platform doesn't
    // reliably deliver back through a plain processEvents() loop for
    // QOpenGLWidget), so a single forced repaint() is enough to guarantee
    // compilePendingShader() has run against whatever is currently pending
    // by the time this returns.
    auto forceViewportRepaint = [&window]() { window.viewportPanelForTesting()->viewportForTesting()->repaint(); };

    window.setLanguageForTesting(shaderfmt::Language::Shadertoy);
    ShaderViewport* viewport = window.viewportPanelForTesting()->viewportForTesting();
    window.editorForTesting()->setText(QString::fromUtf8(
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    vec2 uv = fragCoord / iResolution.xy;\n"
        "    fragColor = vec4(uv, 0.5 + 0.5 * sin(iTime), 1.0);\n"
        "}\n"));
    QCoreApplication::processEvents();
    std::cout << "DIAG-PRE: dirty=" << viewport->sourceDirtyForTesting()
              << " pending=[" << viewport->pendingSourceForTesting().toStdString() << "]\n"
              << std::flush;
    window.triggerViewportUpdateForTesting();
    std::cout << "DIAG-AFTERTRIGGER: dirty=" << viewport->sourceDirtyForTesting()
              << " pending=[" << viewport->pendingSourceForTesting().toStdString() << "]\n"
              << std::flush;
    forceViewportRepaint();
    std::cout << "DIAG-AFTERREPAINT1: dirty=" << viewport->sourceDirtyForTesting()
              << " compileOk=" << viewport->lastCompileSucceededForTesting() << "\n"
              << std::flush;
    forceViewportRepaint();

    std::cout << "DIAG3: compileOk=" << viewport->lastCompileSucceededForTesting()
              << " error=[" << viewport->lastErrorForTesting().toStdString() << "]"
              << " pendingSource=[" << viewport->pendingSourceForTesting().toStdString() << "]"
              << "\n" << std::flush;
    check(viewport->isOpenGLAvailableForTesting(), "viewport gets a real OpenGL context");
    check(viewport->lastCompileSucceededForTesting(),
          "a valid Shadertoy mainImage() compiles and renders in the viewport");
    check(!window.viewportPanelForTesting()->isShowingMessageForTesting(),
          "viewport shows the live render, not a message, once compilation succeeds");

    window.editorForTesting()->setText(QString::fromUtf8(
        "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
        "    this is not valid glsl at all;\n"
        "}\n"));
    QCoreApplication::processEvents();
    window.triggerViewportUpdateForTesting();
    forceViewportRepaint();
    forceViewportRepaint();

    std::cout << "DIAG4: compileOk=" << viewport->lastCompileSucceededForTesting()
              << " error=[" << viewport->lastErrorForTesting().toStdString() << "]"
              << " pendingSource=[" << viewport->pendingSourceForTesting().toStdString() << "]"
              << " showingMsg=" << window.viewportPanelForTesting()->isShowingMessageForTesting()
              << "\n" << std::flush;

    check(!viewport->lastCompileSucceededForTesting(),
          "an invalid shader body fails to compile rather than silently rendering garbage");
    check(!viewport->lastErrorForTesting().isEmpty(), "a failed compile records a non-empty error log");
    check(window.viewportPanelForTesting()->isShowingMessageForTesting(),
          "viewport falls back to the error message when compilation fails");
    check(window.viewportPanelForTesting()->messageTextForTesting().contains(QStringLiteral("Erreur")),
          "the fallback message surfaces the compile error, not a generic placeholder");

    window.setLanguageForTesting(shaderfmt::Language::HLSL);
    window.editorForTesting()->setText(QString::fromUtf8("float4 main() : SV_Target { return 0; }\n"));
    QCoreApplication::processEvents();
    window.triggerViewportUpdateForTesting();
    QCoreApplication::processEvents();
    check(window.viewportPanelForTesting()->isShowingMessageForTesting(),
          "viewport never attempts to render a dialect it doesn't support (HLSL)");
    check(window.viewportPanelForTesting()->messageTextForTesting().contains(QStringLiteral("non disponible")),
          "the unsupported-dialect message is shown, not a stale render/error from before");

    // --- Format action reindents and is idempotent ---
    window.editorForTesting()->setText(QString::fromUtf8("void f(){if(true){int x;}}"));
    QCoreApplication::processEvents();
    window.setLanguageForTesting(shaderfmt::Language::GLSL);
    window.triggerFormatForTesting();
    waitForFormat(window);
    QString firstPass = window.editorForTesting()->text();
    check(firstPass.contains(QStringLiteral("    if")), "Format reindents nested braces");

    window.triggerFormatForTesting();
    waitForFormat(window);
    QString secondPass = window.editorForTesting()->text();
    check(firstPass == secondPass, "Format is idempotent when triggered twice from the UI");

    // --- Large-file formatting doesn't block the GUI thread (roadmap
    // §7): the call that triggers it must return almost immediately,
    // with the actual work (tens of ms for a file this size - see the
    // permanent benchmark in tests/test_main.cpp) happening on a
    // QtConcurrent worker thread instead of synchronously on this one ---
    {
        std::ostringstream ss;
        ss << "#version 450 core\n";
        for (int i = 0; i < 3000; i++) {
            ss << "void fn" << i << "(){ if(true){ int x=" << i << "; } }\n";
        }
        window.editorForTesting()->setText(QString::fromStdString(ss.str()));
        QCoreApplication::processEvents();
        window.setLanguageForTesting(shaderfmt::Language::GLSL);

        QElapsedTimer dispatchTimer;
        dispatchTimer.start();
        window.triggerFormatForTesting();
        qint64 dispatchMs = dispatchTimer.elapsed();
        check(dispatchMs < 20,
              "triggering Format on a large (3000-function) file returns control to the "
              "caller almost immediately instead of blocking on the formatting work");

        waitForFormat(window, 5000);
        check(window.editorForTesting()->text().contains(QStringLiteral("void fn0()")),
              "the large-file format still completes correctly once it settles asynchronously");
    }

    // --- Undo restores the pre-format text in a single step ---
    QString beforeFormat = QStringLiteral("void f(){int x;}");
    window.editorForTesting()->setText(beforeFormat);
    QCoreApplication::processEvents();
    window.triggerFormatForTesting();
    waitForFormat(window);
    check(window.editorForTesting()->text() != beforeFormat, "Format actually changed the text");
    window.editorForTesting()->undo();
    check(window.editorForTesting()->text() == beforeFormat,
          "a single undo fully reverts the format pass");

    // --- Style panel (roadmap §4): changing an option reformats the
    // text already in the editor immediately, no explicit "Formater"
    // click needed ---
    window.setFormatOnFlyForTesting(false);
    window.stylePanelForTesting()->selectPresetForTesting(1); // Default: 4 spaces, same-line braces
    QCoreApplication::processEvents();
    window.editorForTesting()->setText(QStringLiteral("void f(){int x;}"));
    QCoreApplication::processEvents();
    window.stylePanelForTesting()->setIndentWidthForTesting(2);
    waitForFormat(window);
    check(window.editorForTesting()->text().contains(QStringLiteral("void f() {\n  int x;")),
          "changing indent width in the style panel reformats the editor's current text immediately");

    // --- Individual spacing option (roadmap §4/§8): disabling "space
    // after control keyword" reformats immediately too, and re-enabling
    // it restores the default spacing ---
    window.editorForTesting()->setText(QStringLiteral("if(true){int x;}"));
    QCoreApplication::processEvents();
    window.stylePanelForTesting()->setSpaceAfterControlKeywordForTesting(false);
    waitForFormat(window);
    check(window.editorForTesting()->text().contains(QStringLiteral("if(true)")),
          "disabling 'space after control keyword' reformats the editor's current text immediately");
    window.stylePanelForTesting()->setSpaceAfterControlKeywordForTesting(true);
    waitForFormat(window);
    check(window.editorForTesting()->text().contains(QStringLiteral("if (true)")),
          "re-enabling 'space after control keyword' restores the default spacing");

    // Restore the editor to what the next block expects - each style-panel
    // check here is meant to be independently readable, not order-coupled
    // to whatever text a prior block happened to leave behind.
    window.editorForTesting()->setText(QStringLiteral("void f(){int x;}"));
    QCoreApplication::processEvents();

    // --- Presets set multiple options at once and apply them too ---
    window.stylePanelForTesting()->selectPresetForTesting(3); // Unreal: tabs, Allman braces
    waitForFormat(window);
    QString unrealText = window.editorForTesting()->text();
    check(unrealText.contains(QStringLiteral("void f()\n")) && unrealText.contains(QStringLiteral("\n{")),
          "the Unreal preset switches to Allman braces and applies it immediately");
    check(unrealText.contains(QStringLiteral("\tint x;")), "the Unreal preset switches to tabs");

    // --- The live preview reflects the current options, independent of
    // whatever the main editor currently holds ---
    QString previewAfterUnreal = window.stylePanelForTesting()->previewTextForTesting();
    check(previewAfterUnreal.contains(QStringLiteral("\n{")),
          "the style panel's own preview reflects Allman braces once Unreal is selected");

    // --- Preferences persist across a fresh MainWindow (same redirected
    // QSettings path) ---
    window.stylePanelForTesting()->selectPresetForTesting(4); // Unity: spaces, Allman braces
    waitForFormat(window);
    {
        MainWindow secondWindow;
        check(secondWindow.stylePanelForTesting()->options().braceStyle == shaderfmt::BraceStyle::NextLine &&
                  !secondWindow.stylePanelForTesting()->options().useTabs,
              "style preferences (Unity preset) persist into a newly constructed MainWindow");
    }

    // --- Format-on-the-fly: enabling it and typing (debounced) triggers
    // a format pass without an explicit click on "Formater" ---
    window.setFormatOnFlyForTesting(false);
    window.editorForTesting()->setText(QStringLiteral("void g(){int y;}"));
    QCoreApplication::processEvents();
    window.setFormatOnFlyForTesting(true); // toggling on formats once immediately
    waitForFormat(window);
    check(window.editorForTesting()->text().contains(QStringLiteral("    int y;")),
          "enabling format-on-the-fly formats the current text immediately");

    window.editorForTesting()->setText(QStringLiteral("void h(){int z;}"));
    {
        // Debounced on a 500ms QTimer; pump the event loop past that
        // window so the fly-format has a chance to fire, same as it
        // would during a real pause in typing.
        QEventLoop loop;
        QTimer::singleShot(700, &loop, &QEventLoop::quit);
        loop.exec();
    }
    waitForFormat(window); // the debounce timer's own format() call also runs off-thread
    check(window.editorForTesting()->text().contains(QStringLiteral("    int z;")),
          "format-on-the-fly reformats after a pause in typing, with no explicit click");
    window.setFormatOnFlyForTesting(false);

    // --- File load/save round-trip (drag-drop and "Ouvrir..." both funnel
    // into the same loadFile(); "Enregistrer sous..." into saveToFile()) ---
    QTemporaryDir tmpDir;
    check(tmpDir.isValid(), "temp dir for file round-trip test is valid");
    QString filePath = tmpDir.filePath(QStringLiteral("roundtrip.glsl"));
    QString fileContent = QStringLiteral("uniform vec3 uColor;\nvoid main(){gl_FragColor=vec4(uColor,1.0);}\n");
    {
        QFile f(filePath);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
        f.write(fileContent.toUtf8());
    }
    window.loadFileForTesting(filePath);
    QCoreApplication::processEvents();
    check(window.editorForTesting()->text() == fileContent, "loadFile() reads the file's content into the editor");
    check(window.currentLanguageForTesting() == shaderfmt::Language::GLSL,
          "loading a file re-runs auto-detection on its content");

    QString savePath = tmpDir.filePath(QStringLiteral("saved.glsl"));
    window.saveToFile(savePath);
    QFile saved(savePath);
    saved.open(QIODevice::ReadOnly | QIODevice::Text);
    check(QString::fromUtf8(saved.readAll()) == window.editorForTesting()->text(),
          "saveToFile() writes exactly the editor's current text");

    // --- Non-blocking syntax error indicator: an unterminated string
    // should get flagged without any dialog/crash, and be cleared again
    // once the source is fixed ---
    auto anyIndicatorOnLine = [&window](int line, int maxIndex) {
        for (int i = 0; i <= maxIndex; i++) {
            if (window.indicatorValueForTesting(line, i) != 0) return true;
        }
        return false;
    };

    window.setLanguageForTesting(shaderfmt::Language::GLSL);
    window.editorForTesting()->setText(QStringLiteral("int x = \"oops\nint y = 2;"));
    QCoreApplication::processEvents();
    check(anyIndicatorOnLine(0, 13),
          "a lex error (unterminated string) is flagged via a non-blocking indicator, not a popup");

    window.editorForTesting()->setText(QStringLiteral("int x = 1;\n"));
    QCoreApplication::processEvents();
    check(!anyIndicatorOnLine(0, 10), "the indicator clears once the underlying error is fixed");

    // --- Copy to clipboard (private slot, but moc still makes it
    // invokable by name; this exercises the exact same code path the
    // toolbar button's triggered() signal would) ---
    QString marker = QStringLiteral("int marker_value_123;");
    window.editorForTesting()->setText(marker);
    QCoreApplication::processEvents();
    QMetaObject::invokeMethod(&window, "onCopyClicked");
    check(QApplication::clipboard()->text() == marker, "Copy button puts editor text on the clipboard");

    // --- Tabs (roadmap "UI pro" phase 1): each open file is independent -
    // new tab starts blank, switching tabs shows that tab's own text, and
    // formatting/typing in one tab never touches another's content.
    {
        int tabsBefore = window.tabCountForTesting();
        window.editorForTesting()->setText(QStringLiteral("void first_tab(){int a;}"));
        QCoreApplication::processEvents();

        window.newTabForTesting();
        check(window.tabCountForTesting() == tabsBefore + 1, "Nouveau creates one additional tab");
        check(window.editorForTesting()->text().isEmpty(), "a freshly created tab starts with empty text");

        window.setLanguageForTesting(shaderfmt::Language::GLSL);
        window.editorForTesting()->setText(QStringLiteral("void second_tab(){int b;}"));
        QCoreApplication::processEvents();
        window.triggerFormatForTesting();
        waitForFormat(window);
        check(window.editorForTesting()->text().contains(QStringLiteral("second_tab")),
              "the second tab formats its own text independently");

        window.switchToTabForTesting(window.currentTabIndexForTesting() - 1);
        check(window.editorForTesting()->text().contains(QStringLiteral("first_tab")),
              "switching back to the first tab shows its own, untouched text");
        check(!window.editorForTesting()->text().contains(QStringLiteral("second_tab")),
              "the first tab was never touched by formatting the second tab");

        window.switchToTabForTesting(window.currentTabIndexForTesting() + 1);
        check(window.editorForTesting()->text().contains(QStringLiteral("second_tab")),
              "switching forward again lands back on the second tab's own text");

        window.closeTabForTesting(window.currentTabIndexForTesting());
        check(window.tabCountForTesting() == tabsBefore, "closing a tab returns to the prior tab count");
    }

    // --- Version string (roadmap §8): compiled in from CMake's
    // project(VERSION), not hardcoded - a real non-empty "X.Y.Z" is proof
    // configure_file() actually ran and produced Version.hpp.
    QString version = window.versionStringForTesting();
    QRegularExpression versionPattern(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    check(versionPattern.match(version).hasMatch(),
          "App version string is compiled in as X.Y.Z (got: " + version.toStdString() + ")");

    std::cout << (g_failures == 0 ? "self-test: OK\n" : "self-test: FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ShaderFmt"));

    bool smokeTest = false;
    bool selfTest = false;
    std::string screenshotPath;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--smoke-test") == 0) smokeTest = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        if (std::strncmp(argv[i], "--screenshot=", 13) == 0) screenshotPath = argv[i] + 13;
    }

    if (selfTest) {
        return runSelfTest(app);
    }

    // Apply the persisted theme (default: dark) before the window is
    // constructed, so the very first frame is already styled instead of
    // painting once unstyled and then restyling - MainWindow itself only
    // re-applies the stylesheet when the user *toggles* it via the
    // Affichage menu (see MainWindow::onThemeToggled()), it never applies
    // the theme it read at startup.
    {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ShaderFmt"),
                            QStringLiteral("ShaderFmt"));
        bool dark = settings.value(QStringLiteral("theme/dark"), true).toBool();
        shaderfmt_app::applyTheme(app, dark ? shaderfmt_app::ThemeMode::Dark : shaderfmt_app::ThemeMode::Light);
    }

    MainWindow window; // constructor itself sets a default size / restores the last saved layout
    window.show();

    if (!screenshotPath.empty()) {
        window.editorForTesting()->setText(QString::fromUtf8(
            "#version 450 core\nuniform vec3 uLightDir;\nvoid main(){vec3 n=normalize(uLightDir);"
            "if(n.x>0.0){n=-n;}\ngl_FragColor=vec4(n,1.0);}\n"));
        QString path = QString::fromStdString(screenshotPath);
        QTimer::singleShot(200, &app, [&window, path]() {
            window.grab().save(path);
        });
        QTimer::singleShot(400, &app, &QApplication::quit);
    } else if (smokeTest) {
        QTimer::singleShot(300, &app, &QApplication::quit);
    }

    return app.exec();
}
