// Translates the text of the browser's own pages into the interface language, from the catalog the browser controls
// use as well. Text without a translation stays in English.
//
// A page loads "chrome", the text it shares with the browser controls, and its own file, such as "settings". Static
// text in the page's markup is translated with localizeStaticText(). Text a script writes is translated with t() or
// tp() when it's written, and must be written again from onInterfaceLanguageChange(): until the new language's
// catalog has loaded, t() still answers in the previous language, and the listeners run only once it has. Text that's
// added to the page later, or taken out and put back, is translated by calling localizeStaticText() again once it's in
// the page.

let language = "en";
let entries = new Map();
let latestRequest = 0;
let textEntries = [];
let attributeEntries = [];
const listeners = [];
const catalogs = new Map();

function entryKey(context, source) {
    return `${context}\u0004${source}`;
}

// The catalog is data from outside the page, so whatever in it isn't a translation is left out.
async function loadCatalog(requestedLanguage, surfaces) {
    const loaded = new Map();
    for (const surface of surfaces) {
        try {
            const response = await fetch(`resource://ladybird/locale/${requestedLanguage}/${surface}.json`);
            const catalog = await response.json();
            for (const [context, messages] of Object.entries(catalog)) {
                if (typeof messages !== "object" || messages === null || Array.isArray(messages)) continue;
                for (const [source, translation] of Object.entries(messages)) {
                    loaded.set(entryKey(context, source), translation);
                }
            }
        } catch (error) {
            console.warn(`Cannot load the ${surface} interface text for '${requestedLanguage}':`, error);
        }
    }
    return loaded;
}

export async function setInterfaceLanguage(requestedLanguage, surfaces) {
    const supportedLanguage = requestedLanguage === "es" ? "es" : "en";
    const request = ++latestRequest;

    const cacheKey = `${supportedLanguage}:${surfaces.join(",")}`;
    if (!catalogs.has(cacheKey)) {
        catalogs.set(cacheKey, supportedLanguage === "en" ? new Map() : await loadCatalog(supportedLanguage, surfaces));
    }

    // A language asked for later wins over one whose catalog took longer to load.
    if (request !== latestRequest) return;
    if (supportedLanguage === language && catalogs.get(cacheKey) === entries) return;

    language = supportedLanguage;
    entries = catalogs.get(cacheKey);
    applyStaticText();
    for (const listener of listeners) listener(language);
}

export function interfaceLanguage() {
    return language;
}

export function onInterfaceLanguageChange(listener) {
    listeners.push(listener);
}

function findTranslation(source, context, count) {
    const value = entries.get(entryKey(context, source));
    if (typeof value === "string") return value;
    if (typeof value !== "object" || value === null || Array.isArray(value)) return undefined;

    // A form the language has, but the catalog doesn't, is the "other" form.
    if (count !== undefined) {
        const category = new Intl.PluralRules(language).select(count);
        if (Object.hasOwn(value, category) && typeof value[category] === "string") return value[category];
    }
    if (Object.hasOwn(value, "other") && typeof value.other === "string") return value.other;
    return undefined;
}

// Arguments replace {0} to {9}, and {{ and }} are literal braces. A missing argument leaves its placeholder.
function substituteArguments(pattern, args) {
    return pattern.replace(/\{\{|\}\}|\{(\d)\}/g, (match, index) => {
        if (match === "{{") return "{";
        if (match === "}}") return "}";
        return Number(index) < args.length ? String(args[Number(index)]) : match;
    });
}

export function t(source, args = [], context = "") {
    return substituteArguments(findTranslation(source, context) ?? source, args);
}

// The count is argument {0}, formatted for the interface language.
export function tp(count, one, other, args = [], context = "") {
    const allArgs = [new Intl.NumberFormat(language).format(count), ...args];
    const translation = findTranslation(one, context, count);
    return substituteArguments(translation ?? (count === 1 ? one : other), allArgs);
}

const TRANSLATED_ATTRIBUTES = ["aria-label", "placeholder", "title", "data-title"];
const registeredTextNodes = new WeakSet();
const registeredAttributes = new WeakMap();

function isExcluded(element) {
    return element.closest('[translate="no"]') !== null;
}

function contextOf(element) {
    return element.closest("[data-l10n-context]")?.dataset.l10nContext ?? "";
}

function registerAttribute(element, name) {
    const source = element.getAttribute(name);
    if (!source || isExcluded(element)) return;

    let names = registeredAttributes.get(element);
    if (!names) registeredAttributes.set(element, (names = new Set()));
    if (names.has(name)) return;
    names.add(name);

    attributeEntries.push({ element, name, source, context: contextOf(element), translated: false });
}

// Records the English text and the attributes of the given elements, so they can be translated and restored later.
export function localizeStaticText(roots) {
    for (const root of roots) {
        if (!root) continue;

        const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        while (walker.nextNode()) {
            const node = walker.currentNode;
            const parent = node.parentElement;
            const source = node.data.trim().replace(/\s+/g, " ");
            // NB: A node that's already recorded may show a translation now, which must not be taken for its English.
            if (!parent || !source || isExcluded(parent) || registeredTextNodes.has(node)) continue;
            registeredTextNodes.add(node);
            textEntries.push({
                node,
                original: node.data,
                source,
                context: contextOf(parent),
                parent,
                translated: false,
            });
        }

        for (const element of [root, ...root.querySelectorAll("*")]) {
            for (const name of TRANSLATED_ATTRIBUTES) registerAttribute(element, name);
        }
    }
    applyStaticText();
}

export function localizeAttributes(elements, names) {
    for (const element of elements) {
        for (const name of names) registerAttribute(element, name);
    }
    applyStaticText();
}

function markLanguage(element, translated, wasTranslated) {
    if (translated) element.setAttribute("lang", language);
    else if (wasTranslated) element.removeAttribute("lang");
}

// Text that isn't in the page, such as text recorded before it was added, goes back to its English and is forgotten, so
// it can be recorded again once it's in the page.
function forgetDisconnectedText() {
    textEntries = textEntries.filter(entry => {
        if (entry.node.isConnected) return true;
        entry.node.data = entry.original;
        markLanguage(entry.parent, false, entry.translated);
        registeredTextNodes.delete(entry.node);
        return false;
    });
    attributeEntries = attributeEntries.filter(entry => {
        if (entry.element.isConnected) return true;
        entry.element.setAttribute(entry.name, entry.source);
        if (entry.name !== "data-title") markLanguage(entry.element, false, entry.translated);
        registeredAttributes.get(entry.element)?.delete(entry.name);
        return false;
    });
}

function applyStaticText() {
    forgetDisconnectedText();

    for (const entry of textEntries) {
        const translation = findTranslation(entry.source, entry.context);
        const translated = translation !== undefined;
        if (translated) {
            const leading = entry.original.match(/^\s*/)[0];
            const trailing = entry.original.match(/\s*$/)[0];
            entry.node.data = leading + translation + trailing;
        } else {
            entry.node.data = entry.original;
        }
        markLanguage(entry.parent, translated, entry.translated);
        entry.translated = translated;
    }

    for (const entry of attributeEntries) {
        const translation = findTranslation(entry.source, entry.context);
        const translated = translation !== undefined;
        entry.element.setAttribute(entry.name, translated ? translation : entry.source);
        if (entry.name !== "data-title") markLanguage(entry.element, translated, entry.translated);
        entry.translated = translated;
    }
}
