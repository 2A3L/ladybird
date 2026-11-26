import { getByteFormatter } from "../../utils.js";
import { onInterfaceLanguageChange, t } from "../l10n.js";
import { registerDialogDeepLink } from "./dialog-deep-link.js";

const byteFormatter = getByteFormatter(unit => {
    return {
        unitDisplay: unit === "byte" ? "long" : "short",
        maximumFractionDigits: 1,
    };
});

const browsingDataSettings = document.querySelector("#browsing-data-settings");
const browsingDataSettingsClose = document.querySelector("#browsing-data-settings-close");
const browsingDataSettingsDialog = document.querySelector("#browsing-data-settings-dialog");
const browsingDataTotalSize = document.querySelector("#browsing-data-total-size");

const clearBrowsingDataCachedFiles = document.querySelector("#clear-browsing-data-cached-files");
const clearBrowsingDataCachedFilesSize = document.querySelector("#clear-browsing-data-cached-files-size");
const clearBrowsingDataDownloadHistory = document.querySelector("#clear-browsing-data-download-history");
const clearBrowsingDataHistory = document.querySelector("#clear-browsing-data-history");
const clearBrowsingDataRemoveData = document.querySelector("#clear-browsing-data-remove-data");
const clearBrowsingDataSiteData = document.querySelector("#clear-browsing-data-site-data");
const clearBrowsingDataSiteDataSize = document.querySelector("#clear-browsing-data-site-data-size");
const clearBrowsingDataTimeRange = document.querySelector("#clear-browsing-data-time-range");

const globalPrivacyControlToggle = document.querySelector("#global-privacy-control-toggle");

let browsingDataSizes = null;

function loadSettings(settings) {
    if (Object.hasOwn(settings, "globalPrivacyControl")) {
        globalPrivacyControlToggle.checked = settings.globalPrivacyControl;
    }
}

function updateBrowsingDataSizes(sizes) {
    browsingDataSizes = sizes;
    const totalSize = sizes.totalCacheSize + sizes.totalSiteDataSize;

    browsingDataTotalSize.innerText = t("Your browsing data is currently using {0} of disk space", [
        byteFormatter.formatBytes(totalSize),
    ]);

    const cachedFilesSize = byteFormatter.formatBytes(sizes.cacheSizeSinceRequestedTime);
    const siteDataSize = byteFormatter.formatBytes(sizes.siteDataSizeSinceRequestedTime);
    clearBrowsingDataCachedFilesSize.innerText = ` ${t("(remove {0})", [cachedFilesSize])}`;
    clearBrowsingDataSiteDataSize.innerText = ` ${t("(remove {0})", [siteDataSize])}`;
}

function computeTimeRange() {
    const now = Temporal.Now.zonedDateTimeISO();

    switch (clearBrowsingDataTimeRange.value) {
        case "lastHour":
            return now.subtract({ hours: 1 });
        case "last4Hours":
            return now.subtract({ hours: 4 });
        case "today":
            return now.startOfDay();
        case "all":
            return null;
        default:
            console.error(`Unrecognized time range: ${clearBrowsingDataTimeRange.value}`);
            return now;
    }
}

function estimateBrowsingDataSizes() {
    const since = computeTimeRange();

    ladybird.sendMessage("estimateBrowsingDataSizes", {
        since: since?.epochMilliseconds,
    });
}

registerDialogDeepLink({
    hash: "clearBrowsingData",
    tab: "privacy",
    dialog: browsingDataSettingsDialog,
    onOpen: estimateBrowsingDataSizes,
});

browsingDataSettings.addEventListener("click", () => {
    location.hash = "clearBrowsingData";
});

browsingDataSettingsClose.addEventListener("click", () => {
    browsingDataSettingsDialog.close();
});

clearBrowsingDataTimeRange.addEventListener("change", () => {
    estimateBrowsingDataSizes();
});

function setRemoveDataEnabledState() {
    clearBrowsingDataRemoveData.disabled =
        !clearBrowsingDataCachedFiles.checked &&
        !clearBrowsingDataHistory.checked &&
        !clearBrowsingDataDownloadHistory.checked &&
        !clearBrowsingDataSiteData.checked;
}

clearBrowsingDataCachedFiles.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataHistory.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataDownloadHistory.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataSiteData.addEventListener("change", setRemoveDataEnabledState);

clearBrowsingDataRemoveData.addEventListener("click", () => {
    const since = computeTimeRange();

    ladybird.sendMessage("clearBrowsingData", {
        since: since?.epochMilliseconds,
        cachedFiles: clearBrowsingDataCachedFiles.checked,
        history: clearBrowsingDataHistory.checked,
        downloadHistory: clearBrowsingDataDownloadHistory.checked,
        siteData: clearBrowsingDataSiteData.checked,
    });

    browsingDataSettingsDialog.close();
});

globalPrivacyControlToggle.addEventListener("change", () => {
    ladybird.sendMessage("setGlobalPrivacyControl", globalPrivacyControlToggle.checked);
});

onInterfaceLanguageChange(() => {
    if (browsingDataSizes) updateBrowsingDataSizes(browsingDataSizes);
});

document.addEventListener("WebUILoaded", estimateBrowsingDataSizes);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "estimatedBrowsingDataSizes") {
        updateBrowsingDataSizes(event.detail.data);
    } else if (event.detail.name === "clearedBrowsingData") {
        estimateBrowsingDataSizes();
    }
});
