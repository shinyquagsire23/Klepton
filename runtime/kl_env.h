//
// Environment var helper functions
//

#ifndef KL_ENV_H
#define KL_ENV_H

// env var helper: unset gives `dflt`, and "0"/"no"/"off"/"false"/"" are off.
int kl_env_on(const char *name, int dflt);

// env var helper: unset gives `dflt`, set gives the string value
const char *kl_env_str(const char *name, const char *dflt);

// env var helper: unset gives `dflt`, set gives the value via strtol
int kl_env_int(const char *name, int dflt);

// env var helper: unset gives `dflt`, set gives the value via strtol
unsigned int kl_env_uint(const char *name, unsigned int dflt);

// env var helper: unset gives `dflt`, set gives the value via atof
float kl_env_float(const char *name, float dflt);

// Gets KL_PERMISSIVE
int kl_permissive(void);

#endif // KL_ENV_H