import { localizeStaticText, onInterfaceLanguageChange, setInterfaceLanguage, t } from "../l10n.js";

const languageSelect = document.querySelector("#interface-language");

// The English text stays searchable in any language, as the settings are called that elsewhere.
for (const panel of document.querySelectorAll(".tab-panel")) {
    panel.dataset.sourceTitle = panel.dataset.title;
    for (const item of panel.querySelectorAll(".card-title, .card-group, .card-body")) {
        item.dataset.sourceText = item.textContent;
    }
}

// Only static browser-owned text is translated. Text made by the scripts is translated as it is made, and
// user-provided names and site data are left untouched.
localizeStaticText([
    document.querySelector(".settings-sidebar"),
    ...document.querySelectorAll(".tab-panel, dialog"),
    document.querySelector("#search-empty"),
]);

onInterfaceLanguageChange(language => {
    languageSelect.value = language;
    document.title = t("Settings");
});

languageSelect.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", { name: "ui.interface.language", value: languageSelect.value });
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings" && Object.hasOwn(event.detail.data, "configVariables")) {
        languageSelect.value = event.detail.data.configVariables?.["ui.interface.language"] === "es" ? "es" : "en";
        setInterfaceLanguage(event.detail.data.configVariables?.["ui.interface.language"], ["chrome", "settings"]);
    }
});
