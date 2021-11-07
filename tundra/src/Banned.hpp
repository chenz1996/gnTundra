#pragma once

#undef fopen
#define fopen(x,y) fopen__is_banned__use_OpenFile
#undef remove
#define remove(x) remove__is_banned__use_RemoveFileOrDir
#undef rename
#define rename(x,y) rename__is_banned__use_RenameFile
