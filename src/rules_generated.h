/* rules_generated.h — GENERATED FILE, DO NOT EDIT BY HAND.
 *
 * Regenerate with `tools/gen_rules --source <website-checkout> --output
 * src/rules_generated.h`, or `cmake --build build --target rules`.
 *
 * The text is copied verbatim from the website's FAQ (vercel/index.html and
 * vercel/js/i18n.js) rather than paraphrased, and the result is committed so
 * that building the CLI never requires the website checkout.
 *
 * Known sync risk: there is no API endpoint that serves the content rules, so
 * this bundled copy can drift if the site's wording changes. The CI check that
 * regenerating produces no diff is what catches that.
 */
#ifndef ZB_RULES_GENERATED_H
#define ZB_RULES_GENERATED_H

#define ZB_RULES_INTRO                                                         \
    "Short and non-negotiable. Break these and your file is gone, no warning."

typedef struct {
    const char *title;
    const char *detail;
} zb_rule;

static const zb_rule ZB_RULES[] = {
    {"No CSAM, ever.",
     "Zero tolerance. Reported to the relevant authorities immediately."},
    {"No malware, viruses, or exploits.",
     "No ransomware, spyware, phishing kits, or anything built to harm a "
     "device or its owner."},
    {"No stolen data.",
     "Leaked credentials, dumped databases, or doxx files aren't welcome "
     "here."},
    {"No content that breaks the law where you live.",
     "If it would land you in trouble offline, it'll land you banned here."},
    {"Everything else is fair game.",
     "This is a dumb pipe for your files. We'd rather keep it that way."},
};

#define ZB_RULES_COUNT (sizeof(ZB_RULES) / sizeof(ZB_RULES[0]))

#endif /* ZB_RULES_GENERATED_H */
