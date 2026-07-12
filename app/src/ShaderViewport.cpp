#include "ShaderViewport.hpp"

#include <QLabel>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QVector3D>
#include <QVector4D>

namespace {

// Fullscreen triangle from gl_VertexID alone - no vertex buffer needed,
// the standard trick for a single full-viewport draw call.
const char* kVertexSrc = R"(#version 330 core
void main() {
    vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
}
)";

// Bug-audit fix: the viewport's QOpenGLWidget context is requested as
// 3.3 Core Profile (see ShaderViewport::ShaderViewport() below) - a
// deliberate choice, not something to relax: macOS only exposes Core
// Profile for GL >= 3.2 at all (no Compatibility Profile), so lowering
// the profile to accommodate legacy shaders would just break the preview
// on that platform instead of this one. But "gl_FragColor" - the
// implicit fragment output from pre-3.30/compatibility-profile GLSL,
// still very common in real-world shaders (and explicitly one of the
// cases GlslVersionLint tolerates, see its own comments on drivers
// accepting old qualifiers via the compatibility profile) - simply
// doesn't exist in Core Profile. Any otherwise-valid shader using it
// would fail to compile in this preview with a driver error that has
// nothing to do with a real mistake in the shader. Rewritten the same
// way the Shadertoy wrapping just below already rewrites source before
// compiling: replace "gl_FragColor" with a real declared "out vec4"
// variable. Purely a preview-time transformation - never touches the
// text the user actually edits, formats, or copies.
QString adaptLegacyFragColorForCoreProfile(QString source) {
    static const QString kMarker = QStringLiteral("gl_FragColor");
    if (!source.contains(kMarker)) return source;

    static const QString kReplacement = QStringLiteral("shaderfmt_legacyFragColor");
    source.replace(kMarker, kReplacement);
    QString declaration = QStringLiteral("out vec4 ") + kReplacement + QStringLiteral(";\n");

    if (source.startsWith(QStringLiteral("#version"))) {
        int versionLineEnd = source.indexOf(QLatin1Char('\n'));
        source.insert(versionLineEnd >= 0 ? versionLineEnd + 1 : source.size(), declaration);
    } else {
        // No #version directive at all: Core Profile rejects anything
        // below #version 150 outright regardless of gl_FragColor, so
        // compiling with neither fixed would just trade one unrelated
        // driver error for another - inject a reasonable modern default
        // along with the declaration instead.
        source = QStringLiteral("#version 330 core\n") + declaration + source;
    }
    return source;
}

} // namespace

ShaderViewport::ShaderViewport(QWidget* parent) : QOpenGLWidget(parent) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(fmt);

    clock_.start();

    animationTimer_ = new QTimer(this);
    animationTimer_->setInterval(16); // ~60fps
    connect(animationTimer_, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    animationTimer_->start();
}

ShaderViewport::~ShaderViewport() {
    // Destroy GL resources with the context current - required by Qt's
    // QOpenGLWidget contract, otherwise this is a silent resource leak
    // (or, on some drivers, a crash on shutdown).
    makeCurrent();
    delete program_;
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    doneCurrent();
}

void ShaderViewport::setShader(const QString& source) {
    if (source == pendingSource_) return;
    pendingSource_ = source;
    sourceDirty_ = true;
    update();
}

void ShaderViewport::setAnimatingForTesting(bool animating) {
    if (animating) {
        if (!animationTimer_->isActive()) animationTimer_->start();
    } else {
        animationTimer_->stop();
    }
}

void ShaderViewport::initializeGL() {
    initializeOpenGLFunctions();
    openGLAvailable_ = context() != nullptr && context()->isValid();
    if (!openGLAvailable_) return;

    glGenVertexArrays(1, &vao_);
}

void ShaderViewport::resizeGL(int, int) {
    // QOpenGLWidget already calls glViewport(0, 0, w, h) internally
    // before invoking paintGL(); nothing extra needed here.
}

void ShaderViewport::compilePendingShader() {
    if (!openGLAvailable_) {
        lastCompileOk_ = false;
        lastError_ = QStringLiteral("OpenGL is not available on this system.");
        emit compileFinished(false, lastError_);
        return;
    }

    delete program_;
    program_ = new QOpenGLShaderProgram(this);

    bool vOk = program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc);
    bool fOk = program_->addShaderFromSourceCode(QOpenGLShader::Fragment, pendingSource_);
    bool linkOk = vOk && fOk && program_->link();

    lastCompileOk_ = linkOk;
    lastError_ = linkOk ? QString() : program_->log();
    emit compileFinished(lastCompileOk_, lastError_);
}

void ShaderViewport::paintGL() {
    if (!openGLAvailable_) return;

    if (sourceDirty_) {
        compilePendingShader();
        sourceDirty_ = false;
    }

    glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (program_ && lastCompileOk_) {
        program_->bind();
        program_->setUniformValue("iResolution",
            QVector3D(static_cast<float>(width()), static_cast<float>(height()), 1.0f));
        program_->setUniformValue("iTime", static_cast<float>(clock_.elapsed()) / 1000.0f);
        float mouseZ = mouseDown_ ? static_cast<float>(mouseClickPos_.x()) : -static_cast<float>(mouseClickPos_.x());
        float mouseW = mouseDown_ ? static_cast<float>(mouseClickPos_.y()) : -static_cast<float>(mouseClickPos_.y());
        program_->setUniformValue("iMouse",
            QVector4D(static_cast<float>(mousePos_.x()), static_cast<float>(mousePos_.y()), mouseZ, mouseW));
        program_->setUniformValue("iFrame", frame_);

        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        program_->release();
    }

    frame_++;
}

void ShaderViewport::mouseMoveEvent(QMouseEvent* event) {
    // Shadertoy's iMouse convention has its origin bottom-left, matching
    // gl_FragCoord - Qt widget coordinates have theirs top-left.
    mousePos_ = QPointF(event->pos().x(), height() - event->pos().y());
}

void ShaderViewport::mousePressEvent(QMouseEvent* event) {
    mouseDown_ = true;
    mouseClickPos_ = QPointF(event->pos().x(), height() - event->pos().y());
    mousePos_ = mouseClickPos_;
}

void ShaderViewport::mouseReleaseEvent(QMouseEvent*) {
    mouseDown_ = false;
}

ViewportPanel::ViewportPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    stack_ = new QStackedWidget(this);
    viewport_ = new ShaderViewport(stack_);
    messageLabel_ = new QLabel(stack_);
    messageLabel_->setAlignment(Qt::AlignCenter);
    messageLabel_->setWordWrap(true);
    messageLabel_->setMargin(12);

    stack_->addWidget(viewport_);      // index 0: live render
    stack_->addWidget(messageLabel_);  // index 1: unsupported/error message

    layout->addWidget(stack_);

    connect(viewport_, &ShaderViewport::compileFinished, this, &ViewportPanel::onCompileFinished);

    messageLabel_->setText(QStringLiteral("Formatez ou collez un shader GLSL/Shadertoy pour voir l'apercu."));
    stack_->setCurrentWidget(messageLabel_);
}

void ViewportPanel::setShader(const QString& source, shaderfmt::Language lang) {
    if (lang != shaderfmt::Language::GLSL && lang != shaderfmt::Language::Shadertoy) {
        messageLabel_->setText(
            QStringLiteral("Apercu non disponible pour ce langage (GLSL et Shadertoy uniquement)."));
        stack_->setCurrentWidget(messageLabel_);
        return;
    }

    QString wrapped = source;
    if (lang == shaderfmt::Language::Shadertoy) {
        wrapped = QStringLiteral(
                      "#version 330 core\n"
                      "uniform vec3 iResolution;\n"
                      "uniform float iTime;\n"
                      "uniform vec4 iMouse;\n"
                      "uniform int iFrame;\n"
                      "out vec4 shaderfmt_fragColor;\n") +
                  source +
                  QStringLiteral("\nvoid main() { mainImage(shaderfmt_fragColor, gl_FragCoord.xy); }\n");
    }

    wrapped = adaptLegacyFragColorForCoreProfile(wrapped);

    viewport_->setShader(wrapped);
    // Optimistic: show the render immediately. onCompileFinished corrects
    // this back to the message page if compilation actually failed.
    stack_->setCurrentWidget(viewport_);
}

void ViewportPanel::onCompileFinished(bool ok, QString errorLog) {
    if (ok) {
        stack_->setCurrentWidget(viewport_);
    } else {
        messageLabel_->setText(QStringLiteral("Erreur de compilation :\n") + errorLog);
        stack_->setCurrentWidget(messageLabel_);
    }
}

bool ViewportPanel::isShowingMessageForTesting() const {
    return stack_->currentWidget() == static_cast<QWidget*>(messageLabel_);
}

QString ViewportPanel::messageTextForTesting() const {
    return messageLabel_->text();
}
