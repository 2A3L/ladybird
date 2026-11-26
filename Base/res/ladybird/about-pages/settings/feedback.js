import { onInterfaceLanguageChange, t, tp } from "../l10n.js";

const showFeedbackButtonGroup = document.querySelector("#show-feedback-button-group");
const showFeedbackButton = document.querySelector("#show-feedback-button");
const crashModes = [...document.querySelectorAll("input[name=crash-report-sending]")];
const crashReportsWaitingGroup = document.querySelector("#crash-reports-waiting-group");
const crashReportsWaiting = document.querySelector("#crash-reports-waiting");
const crashReportsStatus = document.querySelector("#crash-reports-status");

let crashReportsStatusMessage = "";
let crashReportsWaitingCount = 0;

function renderCrashReportsStatus() {
    crashReportsStatus.textContent = crashReportsStatusMessage ? t(crashReportsStatusMessage) : "";
    crashReportsStatus.hidden = !crashReportsStatusMessage;
}

function renderCrashReportsWaiting() {
    crashReportsWaiting.textContent = tp(crashReportsWaitingCount, "{0} report waiting", "{0} reports waiting");
    crashReportsWaitingGroup.classList.toggle("hidden", crashReportsWaitingCount === 0);
}

function loadSettings(settings) {
    const variable = settings.configVariableDefinitions?.find(
        candidate => candidate.name === "ui.toolbar.show_feedback_button"
    );
    if (variable) {
        showFeedbackButton.checked = variable.value;
        showFeedbackButtonGroup.classList.remove("hidden");
    }

    if (Object.hasOwn(settings, "crashReportSending")) {
        crashModes.forEach(mode => {
            mode.checked = mode.value === settings.crashReportSending;
        });
    }
}

showFeedbackButton.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", {
        name: "ui.toolbar.show_feedback_button",
        value: showFeedbackButton.checked,
    });
});

crashModes.forEach(mode => {
    mode.addEventListener("change", () => ladybird.sendMessage("setCrashReportSending", mode.value));
});

document.querySelector("#review-crash-reports").addEventListener("click", () => {
    location.href = "about:crashes";
});

document.querySelector("#open-crash-reports").addEventListener("click", () => {
    crashReportsStatusMessage = "";
    crashReportsStatus.hidden = true;
    ladybird.sendMessage("showCrashReports");
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadCrashReportsWaiting");
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;
    if (name === "loadSettings") {
        loadSettings(data);
    } else if (name === "crashReportsStatus") {
        crashReportsStatusMessage = data;
        renderCrashReportsStatus();
    } else if (name === "crashReportsWaiting") {
        crashReportsWaitingCount = data;
        renderCrashReportsWaiting();
    }
});

onInterfaceLanguageChange(() => {
    renderCrashReportsStatus();
    if (crashReportsWaitingCount !== 0) renderCrashReportsWaiting();
});
