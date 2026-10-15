/*******************************************************************************
 *
 This file is part of the LibreCAD project, a 2D CAD program

 Copyright (C) 2026 LibreCAD.org
 Copyright (C) 2026 LibreCAD-mcp contributors

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 ******************************************************************************/

#include "lc_remoteapigui.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include "lc_remoteapimodalguard.h"
#include "rs_debug.h"

// How a task travels (design section 1.2):
//
//   server thread                          GUI thread
//   run(): post Task, wait on its future   execute(): gate on depth, refuse on a
//          in 20 ms slices                            user modal, then run fn
//          timeout -> abandon -> 504                  inside a ModalGuard
//          abortWaiting() -> 503
//
// A task's state moves Pending -> Started -> Done on the GUI side. The waiting
// side abandons it with a compare-and-swap: from Pending (the function never
// runs) or from Started (it runs to the end and its result is dropped, with a
// "finished after the client gave up" notice). Lambdas capture the shared
// task by value, so neither side can outlive what it uses.

namespace {

enum TaskState : int { Pending, Started, Done, Abandoned };

struct Task {
    std::function<LC_ApiResponse()> fn;
    std::promise<LC_ApiResponse> promise;
    std::atomic<int> state{Pending};
    QString route;
    LC_RemoteApiGui::ModalPolicy modalPolicy = LC_RemoteApiGui::ModalPolicy::Refuse;
};

// GUI thread only.
int s_depth = 0;
std::function<void(const QString&)> s_lateNotifier;

std::atomic<quint64> s_abortGeneration{0};

constexpr int GateRetryMs = 25;
constexpr int WaitSliceMs = 20;

bool isGuiThread() {
    const QCoreApplication* app = QCoreApplication::instance();
    return app == nullptr || QThread::currentThread() == app->thread();
}

QWidget* userModalWidget(const LC_RemoteApiGui::ModalPolicy policy) {
    if (policy == LC_RemoteApiGui::ModalPolicy::Allow ||
        qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr) {
        return nullptr;
    }
    return QApplication::activeModalWidget();
}

LC_ApiResponse modalRefusal(const QWidget* modal) {
    return LC_ApiError(409, QStringLiteral("gui_modal_open"),
                       QStringLiteral("a dialog is open in LibreCAD"),
                       {{QStringLiteral("window"), modal->windowTitle()},
                        {QStringLiteral("hint"), QStringLiteral("ask the user to close the dialog, then retry")}})
        .toResponse();
}

// Runs the function on the GUI thread: counted in depth(), inside a modal guard,
// with every exception turned into an envelope.
LC_ApiResponse invoke(Task& task) {
    struct DepthScope {
        DepthScope() { ++s_depth; }
        ~DepthScope() { --s_depth; }
    } depthScope;

    LC_ApiResponse response;
    LC_RemoteApiModalGuard guard;
    try {
        response = task.fn();
    }
    catch (const LC_ApiError& e) {
        response = e.toResponse();
    }
    catch (const std::exception& e) {
        LC_ERR << "remote-api: " << task.route << " failed: " << e.what();
        response = LC_ApiError(500, QStringLiteral("internal"), QString::fromLocal8Bit(e.what())).toResponse();
    }
    catch (...) {
        LC_ERR << "remote-api: " << task.route << " failed with an unknown exception";
        response = LC_ApiError(500, QStringLiteral("internal"), QStringLiteral("unknown exception")).toResponse();
    }
    guard.attachTo(response);
    return response;
}

void notifyLate(const QString& route) {
    LC_LOG(RS_Debug::D_WARNING) << "remote-api: " << route << " finished after the client gave up";
    if (s_lateNotifier) {
        s_lateNotifier(route);
    }
}

void execute(const std::shared_ptr<Task>& task) {
    if (task->state.load() != Pending) {
        return; // abandoned before it started: it never runs
    }
    // Re-entrancy gate (the reference's F8): a task posted while another API
    // task pumps events in a nested loop waits until that one has finished.
    if (s_depth > 0) {
        QTimer::singleShot(GateRetryMs, QCoreApplication::instance(), [task] { execute(task); });
        return;
    }
    if (const QWidget* modal = userModalWidget(task->modalPolicy)) {
        int expected = Pending;
        if (task->state.compare_exchange_strong(expected, Done)) {
            task->promise.set_value(modalRefusal(modal));
        }
        return;
    }
    int expected = Pending;
    if (!task->state.compare_exchange_strong(expected, Started)) {
        return;
    }
    LC_ApiResponse response = invoke(*task);
    expected = Started;
    if (task->state.compare_exchange_strong(expected, Done)) {
        task->promise.set_value(std::move(response));
    }
    else {
        notifyLate(task->route);
    }
}

// Abandons a task the waiting side gives up on. Returns the state it found:
// Done means the result is there after all.
int abandon(Task& task) {
    int state = task.state.load();
    while (state == Pending || state == Started) {
        if (task.state.compare_exchange_weak(state, Abandoned)) {
            break;
        }
    }
    return state;
}

} // namespace

namespace LC_RemoteApiGui {

LC_ApiResponse run(std::function<LC_ApiResponse()> fn, const int timeoutMs, const QString& route) {
    return run(std::move(fn), timeoutMs, route, ModalPolicy::Refuse);
}

LC_ApiResponse run(std::function<LC_ApiResponse()> fn, const int timeoutMs, const QString& route,
                   const ModalPolicy modalPolicy) {
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    task->route = route;
    task->modalPolicy = modalPolicy;

    if (isGuiThread()) {
        // Already on the GUI thread (tests, or a caller that is): posting and
        // waiting would deadlock, so run it here, without the gate.
        if (const QWidget* modal = userModalWidget(modalPolicy)) {
            return modalRefusal(modal);
        }
        task->state = Started;
        return invoke(*task);
    }

    std::future<LC_ApiResponse> result = task->promise.get_future();
    const quint64 generation = s_abortGeneration.load();
    QMetaObject::invokeMethod(QCoreApplication::instance(), [task] { execute(task); }, Qt::QueuedConnection);

    const QDeadlineTimer deadline(timeoutMs);
    for (;;) {
        if (result.wait_for(std::chrono::milliseconds(WaitSliceMs)) == std::future_status::ready) {
            return result.get();
        }
        const bool aborted = s_abortGeneration.load() != generation;
        if (!aborted && !deadline.hasExpired()) {
            continue;
        }
        const int found = abandon(*task);
        if (found == Done) {
            return result.get();
        }
        if (aborted) {
            throw LC_ApiError(503, QStringLiteral("shutting_down"), QStringLiteral("the Remote API is stopping"));
        }
        throw LC_ApiError(504, QStringLiteral("ui_timeout"), QStringLiteral("LibreCAD did not answer in time"),
                          {{QStringLiteral("started"), found == Started},
                           {QStringLiteral("hint"),
                            found == Started
                                ? QStringLiteral("the operation is still running and will finish; check the result before retrying")
                                : QStringLiteral("LibreCAD is busy; retry in a few seconds")}});
    }
}

int depth() {
    return s_depth;
}

void abortWaiting() {
    ++s_abortGeneration;
}

void setLateTaskNotifier(std::function<void(const QString& route)> notifier) {
    s_lateNotifier = std::move(notifier);
}

} // namespace LC_RemoteApiGui
