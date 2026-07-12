#pragma once

#include "shaderfmt/Token.hpp"

#include <QElapsedTimer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

class QLabel;

// Live-render preview: renders GLSL/Shadertoy fragment shaders as a
// fullscreen quad with Shadertoy's standard uniforms (iResolution/iTime/
// iMouse/iFrame), animated continuously via a QTimer. Deliberately does
// NOT attempt to render HLSL/WGSL/MSL/ShaderLab - each needs its own
// real graphics backend (Direct3D/wgpu/Metal/the Unity engine itself),
// none of which are wired up here; those show a plain "not available"
// message instead of a fake or silently-wrong render.
class ShaderViewport : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    explicit ShaderViewport(QWidget* parent = nullptr);
    ~ShaderViewport() override;

    void setShader(const QString& source);

    // Test/automation hooks (mirrors MainWindow/StylePanel's *ForTesting()
    // pattern): a real OpenGL context - possibly software-rasterized via
    // Qt's bundled opengl32sw.dll, since this environment/CI runners have
    // no GPU - is what's actually exercised, not a mock.
    bool isOpenGLAvailableForTesting() const { return openGLAvailable_; }
    bool lastCompileSucceededForTesting() const { return lastCompileOk_; }
    QString lastErrorForTesting() const { return lastError_; }
    QString pendingSourceForTesting() const { return pendingSource_; }
    bool sourceDirtyForTesting() const { return sourceDirty_; }
    void setAnimatingForTesting(bool animating);
    int frameCountForTesting() const { return frame_; }

signals:
    void compileFinished(bool ok, QString errorLog);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void compilePendingShader();

    QOpenGLShaderProgram* program_ = nullptr;
    unsigned int vbo_ = 0;
    unsigned int vao_ = 0;

    QString pendingSource_;
    bool sourceDirty_ = false;

    bool openGLAvailable_ = false;
    bool lastCompileOk_ = false;
    QString lastError_;

    QElapsedTimer clock_;
    QTimer* animationTimer_ = nullptr;
    QPointF mousePos_;
    QPointF mouseClickPos_;
    bool mouseDown_ = false;
    int frame_ = 0;
};

// Wraps ShaderViewport with a "preview not available" fallback page
// (wrong/unsupported language, or a shader that fails to compile) so the
// GL widget is never shown in a state that looks like it's rendering
// when it isn't.
class ViewportPanel : public QWidget {
    Q_OBJECT
public:
    explicit ViewportPanel(QWidget* parent = nullptr);

    // No-op (shows the "not supported" message) unless lang is GLSL or
    // Shadertoy - see the class comment on ShaderViewport for why.
    void setShader(const QString& source, shaderfmt::Language lang);

    // Test/automation hooks.
    ShaderViewport* viewportForTesting() { return viewport_; }
    bool isShowingMessageForTesting() const;
    QString messageTextForTesting() const;

private slots:
    void onCompileFinished(bool ok, QString errorLog);

private:
    QStackedWidget* stack_ = nullptr;
    ShaderViewport* viewport_ = nullptr;
    QLabel* messageLabel_ = nullptr;
};
