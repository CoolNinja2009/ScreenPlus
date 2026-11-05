/* True while a fullscreen window is showing on the current virtual desktop, on any screen. */
import QtQuick
import org.kde.taskmanager as TaskManager

Item {
    property bool fullscreen: false

    function check() {
        let on = false
        for (let i = 0; i < tasks.count && !on; i++) {
            const idx = tasks.makeModelIndex(i)
            on = tasks.data(idx, TaskManager.AbstractTasksModel.IsFullScreen) === true
              && tasks.data(idx, TaskManager.AbstractTasksModel.IsMinimized) !== true
        }
        fullscreen = on
    }

    TaskManager.VirtualDesktopInfo { id: desktops }

    TaskManager.TasksModel {
        id: tasks
        groupMode: TaskManager.TasksModel.GroupDisabled
        virtualDesktop: desktops.currentDesktop
        filterByVirtualDesktop: true
        onDataChanged: check()
        onCountChanged: check()
    }

    Component.onCompleted: check()
}
