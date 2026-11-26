import { onInterfaceLanguageChange, t } from "../l10n.js";

const contentBlockerLists = document.querySelector("#content-blocker-lists");
const languageLists = document.querySelector("#language-content-blocker-lists");
const updateLists = document.querySelector("#update-content-blocker-lists");
const addFilterList = document.querySelector("#add-filter-list");
const addFilterListForm = document.querySelector("#add-filter-list-form");
const subscriptionUrl = document.querySelector("#custom-content-blocker-subscription-url");
const addSubscription = document.querySelector("#add-content-blocker-subscription");
const subscriptionStatus = document.querySelector("#custom-content-blocker-subscription-status");
const localListPicker = document.querySelector("#local-content-blocker-list-picker");
const importLocalList = document.querySelector("#import-local-content-blocker-list");
const localListStatus = document.querySelector("#local-content-blocker-list-status");
const disabledSitesSection = document.querySelector("#content-blocker-disabled-sites-section");
const disabledSites = document.querySelector("#content-blocker-disabled-sites");
const customFilters = document.querySelector("#custom-content-blocker-filters");
const saveCustomFilters = document.querySelector("#save-custom-content-blocker-filters");
const customFiltersStatus = document.querySelector("#custom-content-blocker-filters-status");
const enableContentBlocking = document.querySelector("#enable-content-blocking");

const maximumCustomFilterSize = 4 * 1024 * 1024;

let updateTimer;
let savedCustomFilters = "";
let updateInProgress = false;
let currentLists = [];
let currentDisabledSites = [];

// Each status shows a message that is translated again when the interface language changes.
const statusMessages = new Map();

function showStatus(element, render) {
    statusMessages.set(element, render);
    element.innerText = render();
}

// The browser reports the outcome of an import or a subscription in English, and names the list or the reason.
function translateResult(message) {
    const imported = message.match(/^(.*) imported\.$/s);
    if (imported) return t("{0} imported.", [imported[1]]);
    const failed = message.match(/^Unable to import list: (.*)$/s);
    if (failed) return t("Unable to import list: {0}", [failed[1]]);
    return t(message);
}

function createListRow(list, removable = false) {
    const row = document.querySelector("#filter-list-row").content.firstElementChild.cloneNode(true);
    const toggle = row.querySelector("input");
    const label = row.querySelector("label");
    const labelText = row.querySelector(".label");
    const details = row.querySelector(".description");

    const id = `content-blocker-list-${list.identifier}`;
    const name = list.name ?? list.url;

    toggle.id = id;
    label.htmlFor = id;
    labelText.textContent = name;

    // A list added by the user has no description of its own, so it is described by where it comes from. Only the
    // description of a built-in list is the browser's text.
    if (list.builtIn) {
        details.textContent = t(list.description);
    } else {
        details.textContent = list.description || (list.url ? new URL(list.url).host : t("Local file"));
    }

    toggle.checked = list.enabled;
    toggle.addEventListener("change", () => {
        ladybird.sendMessage("setContentBlockerListEnabled", { identifier: list.identifier, enabled: toggle.checked });
    });

    const remove = row.querySelector("button");
    if (removable) {
        remove.textContent = t("Remove");
        remove.setAttribute("aria-label", t("Remove {0}", [name]));
        remove.addEventListener("click", () => ladybird.sendMessage("removeContentBlockerList", list.identifier));
    } else {
        remove.remove();
    }

    return row;
}

function createDisabledSiteRow(site) {
    const row = document.createElement("div");

    const tile = document.createElement("span");
    tile.className = "letter-tile";
    tile.setAttribute("aria-hidden", "true");
    tile.textContent = site.charAt(0).toUpperCase();

    const name = document.createElement("span");
    name.className = "item-name";
    name.textContent = site;

    const turnBackOn = document.createElement("button");
    turnBackOn.className = "link-button";
    turnBackOn.textContent = t("Turn back on");
    turnBackOn.setAttribute("aria-label", t("Turn content blocking back on for {0}", [site]));
    turnBackOn.addEventListener("click", () => {
        ladybird.sendMessage("setContentBlockerEnabledForSite", { site, enabled: true });
    });

    row.append(tile, name, turnBackOn);
    return row;
}

function renderLists() {
    contentBlockerLists.replaceChildren();
    languageLists.replaceChildren();

    for (const list of currentLists) {
        const container = list.builtIn && list.languageSpecific ? languageLists : contentBlockerLists;
        container.append(createListRow(list, !list.builtIn));
    }
}

function renderDisabledSites() {
    disabledSites.replaceChildren(...currentDisabledSites.map(createDisabledSiteRow));
    disabledSitesSection.hidden = currentDisabledSites.length === 0;
}

function renderUpdateButton() {
    updateLists.innerText = updateInProgress ? t("Updating…") : t("Update enabled lists");
}

function loadSettings(settings) {
    if (Object.hasOwn(settings, "contentBlockers")) {
        const contentBlockers = settings.contentBlockers;
        enableContentBlocking.checked = contentBlockers.enabled;

        if (customFilters.value === savedCustomFilters) {
            customFilters.value = contentBlockers.customFilters;
        }

        savedCustomFilters = contentBlockers.customFilters;

        currentDisabledSites = [...(contentBlockers.disabledSites ?? [])].sort();
        renderDisabledSites();
    }

    if (Object.hasOwn(settings, "contentBlockerLists")) {
        currentLists = settings.contentBlockerLists;
        renderLists();
    }

    if (Object.hasOwn(settings, "contentBlockerListUpdateInProgress")) {
        updateInProgress = settings.contentBlockerListUpdateInProgress;
        updateLists.disabled = updateInProgress;
        renderUpdateButton();

        clearInterval(updateTimer);
        if (settings.contentBlockerListUpdateInProgress) {
            updateTimer = setInterval(() => ladybird.sendMessage("refreshSettings"), 1000);
        }
    }
}

addFilterList.addEventListener("click", () => {
    addFilterListForm.hidden = !addFilterListForm.hidden;
    addFilterList.setAttribute("aria-expanded", String(!addFilterListForm.hidden));
    if (!addFilterListForm.hidden) subscriptionUrl.focus();
});

enableContentBlocking.addEventListener("change", () => {
    ladybird.sendMessage("setContentBlockerEnabled", enableContentBlocking.checked);
});

addSubscription.addEventListener("click", () => {
    subscriptionUrl.value = ensureURLHasScheme(subscriptionUrl.value);
    if (!subscriptionUrl.reportValidity() || subscriptionUrl.value.length === 0) {
        return;
    }
    const url = new URL(subscriptionUrl.value);
    if (url.protocol !== "http:" && url.protocol !== "https:") {
        showStatus(subscriptionStatus, () => t("Enter an HTTP or HTTPS URL."));
        return;
    }
    ladybird.sendMessage("addCustomContentBlockerSubscription", subscriptionUrl.value);
    subscriptionUrl.value = "";
});

updateLists.addEventListener("click", () => {
    ladybird.sendMessage("updateContentBlockerLists");
});

importLocalList.addEventListener("click", () => localListPicker.click());
localListPicker.addEventListener("change", async () => {
    const file = localListPicker.files[0];
    if (!file) {
        return;
    }
    if (file.size > 64 * 1024 * 1024) {
        showStatus(localListStatus, () => t("The selected list is larger than 64 MiB."));
        localListPicker.value = "";
        return;
    }
    const data = {
        name: file.name,
        contents: await file.text(),
    };
    // IPC's JSON serializer uses six-byte escapes for carriage returns and form feeds.
    const serialized = JSON.stringify(data, (_, value) =>
        typeof value === "string" ? value.replace(/[\r\f]/g, "\0") : value
    );
    // Leave room for the IPC message headers and method name.
    const maximumPayloadSize = 64 * 1024 * 1024 - 1024;
    if (
        serialized.length > maximumPayloadSize ||
        new TextEncoder().encode(serialized).byteLength > maximumPayloadSize
    ) {
        showStatus(localListStatus, () => t("The encoded list is too large to import."));
        localListPicker.value = "";
        return;
    }
    ladybird.sendMessage("importLocalContentBlockerList", data);
    localListPicker.value = "";
});

saveCustomFilters.addEventListener("click", () => {
    if (new TextEncoder().encode(customFilters.value).byteLength > maximumCustomFilterSize) {
        showStatus(customFiltersStatus, () => t("Custom filters must be 4 MiB or smaller."));
        customFilters.focus();
        return;
    }
    ladybird.sendMessage("setCustomContentBlockerFilters", customFilters.value);
    showStatus(customFiltersStatus, () => t("Saved"));
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "contentBlockerResult") {
        const result = event.detail.data;
        const status = result.operation === "subscription" ? subscriptionStatus : localListStatus;
        showStatus(status, () => translateResult(result.message));
    }
});

onInterfaceLanguageChange(() => {
    renderLists();
    renderDisabledSites();
    renderUpdateButton();
    for (const [element, render] of statusMessages) element.innerText = render();
});
