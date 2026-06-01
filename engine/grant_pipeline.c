/**
 * grant_pipeline.c — R31: Grant Pipeline Tracker (replaces 141-line Python)
 * Tracks grant deadlines, calculates days remaining, surfaces next actions.
 * Build: gcc -O2 grant_pipeline.c -o grant_pipeline -lm
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

typedef struct { const char *id, *name, *amount, *deadline, *url, *status, *proposal_path, *notes; } Grant;

static const Grant GRANTS[] = {
    {"nlnet-ngi-zero", "NLnet NGI Zero Commons Fund", "e25,000", "2026-06-01",
     "https://nlnet.nl/propose/", "proposal_written",
     "~/.hermes/mind-palace/plans/nlnet_grant_proposal.md",
     "Call-based. Next: June 1, 2026. Proposal written, needs user submission."},
    {"eleutherai-soar", "EleutherAI SOAR 2026", "Stipend + $75K compute", "2026-06-08",
     "https://www.eleuther.ai/soar", "needs_proposal", "",
     "5-week online research program. Need 1-2 page research proposal."},
    {"gitcoin-grants", "Gitcoin Grants (next round TBD)", "Variable (QF matching)", "",
     "https://gitcoin.co/grants", "monitoring", "",
     "Ongoing rounds. Monitor for next round announcement."},
    {"optimism-retropgf", "Optimism RetroPGF Round 5", "Variable", "",
     "https://app.optimism.io/retropgf", "monitoring", "",
     "Next round TBD. Tracked for announcement."},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

static int parse_date(const char *s, struct tm *tm) {
    if (!s || !*s) return -1;
    memset(tm, 0, sizeof(*tm));
    if (sscanf(s, "%d-%d-%d", &tm->tm_year, &tm->tm_mon, &tm->tm_mday) != 3) return -1;
    tm->tm_year -= 1900;
    tm->tm_mon -= 1;
    return 0;
}

static int days_between(const char *deadline_str) {
    struct tm dl_tm;
    if (parse_date(deadline_str, &dl_tm) != 0) return -999;
    time_t dl = mktime(&dl_tm);
    time_t now = time(NULL);
    // Normalize to start of day
    struct tm *now_tm = localtime(&now);
    now_tm->tm_hour = 0; now_tm->tm_min = 0; now_tm->tm_sec = 0;
    time_t today = mktime(now_tm);
    return (int)round((difftime(dl, today)) / 86400.0);
}

int main(void) {
    int urgent_count = 0;
    char output[8192]; output[0] = 0;

    // Header
    strcat(output, "Grant Pipeline - ");

    for (int i = 0; GRANTS[i].name; i++) {
        int days = days_between(GRANTS[i].deadline);
        const char *emoji = "?";
        if (strcmp(GRANTS[i].status, "proposal_written") == 0) emoji = "P";
        else if (strcmp(GRANTS[i].status, "needs_proposal") == 0) emoji = "W";
        else if (strcmp(GRANTS[i].status, "monitoring") == 0) emoji = "E";
        else if (strcmp(GRANTS[i].status, "submitted") == 0) emoji = "Y";

        char deadline_str[64];
        if (!GRANTS[i].deadline || !*GRANTS[i].deadline) {
            snprintf(deadline_str, sizeof(deadline_str), "TBD");
        } else if (days < 0) {
            snprintf(deadline_str, sizeof(deadline_str), "PAST by %dd", -days);
        } else if (days == 0) {
            snprintf(deadline_str, sizeof(deadline_str), "DUE TODAY");
        } else if (days <= 3) {
            snprintf(deadline_str, sizeof(deadline_str), "%dd left", days);
        } else if (days <= 7) {
            snprintf(deadline_str, sizeof(deadline_str), "%dd left", days);
        } else if (days <= 14) {
            snprintf(deadline_str, sizeof(deadline_str), "%dd left", days);
        } else {
            snprintf(deadline_str, sizeof(deadline_str), "%dd away", days);
        }

        char line[512];
        snprintf(line, sizeof(line), "%s **%s** - %s - %s\n   %s\n",
                 emoji, GRANTS[i].name, GRANTS[i].amount, deadline_str, GRANTS[i].url);
        strncat(output, line, sizeof(output) - strlen(output) - 1);
        if (GRANTS[i].proposal_path && *GRANTS[i].proposal_path) {
            char p[128]; snprintf(p, sizeof(p), "   File: %s\n", GRANTS[i].proposal_path);
            strncat(output, p, sizeof(output) - strlen(output) - 1);
        }
        if (GRANTS[i].notes && *GRANTS[i].notes) {
            char n[256]; snprintf(n, sizeof(n), "   Note: %s\n", GRANTS[i].notes);
            strncat(output, n, sizeof(output) - strlen(output) - 1);
        }
        strncat(output, "\n", sizeof(output) - strlen(output) - 1);

        if (days >= 0 && days <= 14) urgent_count++;
    }

    // Find next deadline
    int next_idx = -1, min_days = 9999;
    for (int i = 0; GRANTS[i].name; i++) {
        int d = days_between(GRANTS[i].deadline);
        if (d >= 0 && d < min_days) { min_days = d; next_idx = i; }
    }

    if (urgent_count > 0) {
        printf("URGENT: %d grant deadline(s) approaching\n\n", urgent_count);
    } else {
        printf("No urgent deadlines\n\n");
    }
    printf("%s", output);

    if (next_idx >= 0) {
        const Grant *g = &GRANTS[next_idx];
        printf("**Next action:**\n");
        if (strcmp(g->status, "proposal_written") == 0) {
            printf("  %s (%dd left) - Proposal written. Needs submission at %s\n",
                   g->name, min_days, g->url);
        } else if (strcmp(g->status, "needs_proposal") == 0) {
            printf("  %s (%dd left) - Needs proposal written\n", g->name, min_days);
        } else {
            printf("  %s (%dd left)\n", g->name, min_days);
        }
    }
    return 0;
}
