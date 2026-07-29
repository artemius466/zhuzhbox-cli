#include "store/paths.h"

#include "util/buf.h"
#include "util/platform.h"

#define ZB_APP_DIR_NAME "zhuzhbox"

char *zb_paths_base_dir(const char *override_dir)
{
    char *root;
    char *base;

    if (override_dir != NULL && override_dir[0] != '\0') {
        return zb_strdup(override_dir);
    }

    {
        char *env = zb_getenv_dup("ZHUZHBOX_CONFIG_DIR");
        if (env != NULL) {
            return env;
        }
    }

    root = zb_user_config_root();
    if (root == NULL) {
        return NULL;
    }
    base = zb_path_join(root, ZB_APP_DIR_NAME);
    zb_free(root);
    return base;
}

int zb_paths_ensure_dir(const char *base_dir)
{
    if (base_dir == NULL) {
        return -1;
    }
    return zb_mkdir_p(base_dir);
}

char *zb_paths_file(const char *base_dir, const char *name)
{
    return zb_path_join(base_dir, name);
}
