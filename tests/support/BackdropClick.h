#pragma once

// What the three panel tests share about the backdrop behind a panel: the properties a backdrop has to carry to
// cover the output without taking the keyboard, and a real click delivered to it.
//
// The window is offscreen and never configured by a compositor, so an all-edges surface has no size of its own; it
// is given one here, and the click is a real window event (QTest::mouseClick) so the `MouseArea` in `Backdrop.qml`
// is what answers it rather than a signal emitted by the test.
#include "wayland/LayerShellWindow.h"

#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWindow>

namespace backdrop {

inline void verifyIsABackdrop(QWindow* window)
{
    QVERIFY(window != nullptr);
    QCOMPARE(window->property("layerNamespace").toString(), QStringLiteral("quantum-shell-backdrop"));
    // One layer below the panels' overlay layer: the protocol's order is what puts a panel above its backdrop.
    QCOMPARE(window->property("layer").toInt(), static_cast<int>(QuantumShell::LayerShellWindow::Top));
    QCOMPARE(window->property("anchors").toInt(),
             static_cast<int>(QuantumShell::LayerShellWindow::TopEdge | QuantumShell::LayerShellWindow::BottomEdge |
                              QuantumShell::LayerShellWindow::LeftEdge | QuantumShell::LayerShellWindow::RightEdge));
    QCOMPARE(window->property("keyboardInteractivity").toInt(),
             static_cast<int>(QuantumShell::LayerShellWindow::NoKeyboard));
    QCOMPARE(window->property("exclusiveZone").toInt(), -1);
}

// Delivers a left click in the middle of the window, after giving it a size the compositor would have.
inline void click(QWindow* window)
{
    auto* quick = qobject_cast<QQuickWindow*>(window);
    QVERIFY(quick != nullptr);
    // A resize by a pixel and back is the event a compositor's configure is; the content item is 0x0 until the
    // window system's resize has been delivered, and a click cannot land on an item that has no size.
    quick->resize(401, 300);
    quick->resize(400, 300);
    QVERIFY2(QTest::qWaitFor([&] { return quick->contentItem()->width() == 400; }, 3000),
             "the backdrop's content item was never sized");
    QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
}

}  // namespace backdrop
