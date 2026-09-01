#include "settings.h"
#include "../include/config.h"
#include <Preferences.h>
#include <string.h>

static Preferences s_prefs;

static char s_apiBase[64];
static char s_deviceToken[64];
static char s_smsPrimary[20];
static char s_smsSecondary[20];

static void load(const char* key, char* out, size_t cap, const char* def) {
    String v = s_prefs.getString(key, def);
    strncpy(out, v.c_str(), cap - 1);
    out[cap - 1] = 0;
}

static void save(const char* key, char* cache, size_t cap, const char* v) {
    strncpy(cache, v, cap - 1);
    cache[cap - 1] = 0;
    s_prefs.putString(key, cache);
}

namespace settings {

void begin() {
    s_prefs.begin("scanner", false);
    load("api_base",      s_apiBase,      sizeof s_apiBase,      DEFAULT_API_BASE);
    load("token",         s_deviceToken,  sizeof s_deviceToken,  DEFAULT_DEVICE_TOKEN);
    load("sms_primary",   s_smsPrimary,   sizeof s_smsPrimary,   DEFAULT_SMS_PARENT_PRIMARY);
    load("sms_secondary", s_smsSecondary, sizeof s_smsSecondary, DEFAULT_SMS_PARENT_SECONDARY);
}

const char* apiBase()      { return s_apiBase; }
const char* deviceToken()  { return s_deviceToken; }
const char* smsPrimary()   { return s_smsPrimary; }
const char* smsSecondary() { return s_smsSecondary; }

void setApiBase(const char* v)      { save("api_base",      s_apiBase,      sizeof s_apiBase, v); }
void setDeviceToken(const char* v)  { save("token",         s_deviceToken,  sizeof s_deviceToken, v); }
void setSmsPrimary(const char* v)   { save("sms_primary",   s_smsPrimary,   sizeof s_smsPrimary, v); }
void setSmsSecondary(const char* v) { save("sms_secondary", s_smsSecondary, sizeof s_smsSecondary, v); }

}
