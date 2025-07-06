#include "../dew.h"
#include "lutil.h"
#include <dirent.h>

// fun fs_readdir(path str) [str]
int l_fs_readdir(lua_State* L) {
    usize path_len;
    const char* path = luaL_checklstring(L, 1, &path_len);
    if (!path)
        return 0;

    DIR* dp = opendir(path);
    if (dp == NULL)
        return l_errno_error(L, errno);

    struct dirent* d;
    lua_createtable(L, 8, 0);
    int i = 1;

    while ((d = readdir(dp)) != NULL) {
        if (*d->d_name == '.')
            continue;
        lua_pushstring(L, d->d_name);
        lua_rawseti(L, -2, i++);
    }

    closedir(dp);
    return 1;
}
