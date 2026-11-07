/* What covers this display on the current virtual desktop: a fullscreen window, or any maximised one; and
 * whether the active window here is one of the app rules' pause apps (PauseApps: "name;name", lowercase, as
 * config.c keeps them). An app matches by its app id or name: "dolphin" matches org.kde.dolphin. */
import QtQuick
import QtQuick.Window
import org.kde.taskmanager as TaskManager

Item {
    property bool fullscreen: false
    property bool covered: false
    property bool ruleApp: false
    property string rules: ""

    onRulesChanged: check()

    function listed(id, name) {
        const ids = [String(id || "").toLowerCase(), String(name || "").toLowerCase()]
        for (const r of rules.split(";")) {
            const stem = r.replace(/\.exe$/, "")
            if (!stem) continue
            for (const v of ids)
                if (v && (v === stem || v.endsWith("." + stem) || v === r)) return true
        }
        return false
    }

    function check() {
        let on = false, over = false, app = false
        for (let i = 0; i < tasks.count; i++) {
            const idx = tasks.makeModelIndex(i)
            if (tasks.data(idx, TaskManager.AbstractTasksModel.IsMinimized) === true) continue
            const full = tasks.data(idx, TaskManager.AbstractTasksModel.IsFullScreen) === true
            on = on || full
            over = over || full || tasks.data(idx, TaskManager.AbstractTasksModel.IsMaximized) === true
            if (rules && tasks.data(idx, TaskManager.AbstractTasksModel.IsActive) === true)
                app = listed(tasks.data(idx, TaskManager.AbstractTasksModel.AppId), tasks.data(idx, Qt.DisplayRole))
        }
        fullscreen = on
        covered = over
        ruleApp = app
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
