/* What covers this display on the current virtual desktop: a fullscreen window, or any maximised one. */
import QtQuick
import QtQuick.Window
import org.kde.taskmanager as TaskManager

Item {
    property bool fullscreen: false
    property bool covered: false

    function check() {
        let on = false, over = false
        for (let i = 0; i < tasks.count; i++) {
            const idx = tasks.makeModelIndex(i)
            if (tasks.data(idx, TaskManager.AbstractTasksModel.IsMinimized) === true) continue
            const full = tasks.data(idx, TaskManager.AbstractTasksModel.IsFullScreen) === true
            on = on || full
            over = over || full || tasks.data(idx, TaskManager.AbstractTasksModel.IsMaximized) === true
        }
        fullscreen = on
        covered = over
    }

    TaskManager.VirtualDesktopInfo { id: desktops }

    TaskManager.TasksModel {
        id: tasks
        groupMode: TaskManager.TasksModel.GroupDisabled
        virtualDesktop: desktops.currentDesktop
        filterByVirtualDesktop: true
        screenGeometry: Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)
        filterByScreen: true
        onDataChanged: check()
        onCountChanged: check()
    }

    Component.onCompleted: check()
}
