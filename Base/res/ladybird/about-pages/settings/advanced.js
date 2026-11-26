import { onInterfaceLanguageChange, t } from "../l10n.js";
import { normalizeSearchText } from "./search-text.js";

const configFilter = document.querySelector("#config-filter");
const configList = document.querySelector("#config-list");

const CONFIG_VARIABLES = new Map();

function sendConfigVariableValue(variable, value) {
    ladybird.sendMessage("setConfigVariable", {
        name: variable.name,
        value,
    });
}

function createControl(variable) {
    const type = variable.type === "array" && variable.elementType === "string" ? "textarea" : "input";
    const input = document.createElement(type);
    input.id = variable.name;
    // NB: Configuration values are names, numbers and addresses, not words to check the spelling of.
    input.spellcheck = false;

    if (variable.type === "boolean") {
        input.type = "checkbox";
        input.switch = true;
        input.checked = !!variable.value;
        input.addEventListener("change", () => {
            sendConfigVariableValue(variable, input.checked);
        });

        return input;
    }

    if (type === "textarea") {
        input.rows = 3;
        input.value = Array.isArray(variable.value) ? variable.value.join("\n") : "";
        input.addEventListener("change", () => {
            const value = input.value
                .split(/\r?\n/)
                .map(entry => entry.trim())
                .filter(entry => entry.length !== 0);

            sendConfigVariableValue(variable, value);
        });

        return input;
    }

    input.value = variable.value ?? "";

    if (variable.type === "number") {
        input.type = "number";
        input.step = "any";
        input.addEventListener("change", () => {
            const value = Number.parseFloat(input.value);
            if (!Number.isNaN(value)) {
                sendConfigVariableValue(variable, value);
            }
        });
    } else {
        input.type = "text";
        input.addEventListener("change", () => {
            sendConfigVariableValue(variable, input.value);
        });
    }

    return input;
}

// The titles and descriptions come from the browser in English. The English text stays searchable.
function setRowText(row, title, description, variable) {
    title.textContent = t(variable.title);
    description.textContent = t(variable.description);
    row.dataset.filterText = normalizeSearchText(
        `${title.textContent} ${description.textContent} ${variable.title} ${variable.description}`
    );
}

function createRow(variable) {
    const row = document.createElement("div");
    row.classList.add("card-group");
    row.classList.add("config-row");
    row.classList.add("inline-container");
    row.classList.add("control-row");

    const title = document.createElement("p");
    const description = document.createElement("p");
    description.classList.add("description");
    setRowText(row, title, description, variable);

    const label = document.createElement("label");
    label.htmlFor = variable.name;
    label.append(title, description);

    row.append(label, createControl(variable));
    return row;
}

function applyFilter() {
    const filter = normalizeSearchText(configFilter.value.trim());

    document.querySelectorAll(".config-row").forEach(row => {
        row.classList.toggle("hidden", filter.length !== 0 && !row.dataset.filterText.includes(filter));
    });
}

function loadConfigVariables(settings) {
    if (!Object.hasOwn(settings, "configVariableDefinitions")) {
        return;
    }

    const configVariables = settings.configVariableDefinitions || [];
    configVariables.forEach(variable => {
        // This one has its own switch on the Feedback tab.
        if (variable.name === "ui.toolbar.show_feedback_button") {
            return;
        }

        const previous = CONFIG_VARIABLES.get(variable.name);
        if (previous && JSON.stringify(previous.value) === JSON.stringify(variable.value)) {
            return;
        }

        CONFIG_VARIABLES.set(variable.name, variable);

        const row = createRow(variable);
        const existing = document.getElementById(variable.name)?.closest(".config-row");
        if (existing) {
            existing.replaceWith(row);
        } else {
            configList.append(row);
        }
    });

    applyFilter();
}

configFilter.addEventListener("input", applyFilter);

onInterfaceLanguageChange(() => {
    for (const variable of CONFIG_VARIABLES.values()) {
        const row = document.getElementById(variable.name)?.closest(".config-row");
        if (row) setRowText(row, ...row.querySelectorAll("label > p"), variable);
    }
    applyFilter();
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadConfigVariables(event.detail.data);
    }
});
