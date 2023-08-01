/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 */

#include "BKE_context.h"

#include "ED_fileselect.h"
#include "ED_screen.h"

#include "RNA_prototypes.h"

#include "file_intern.h"
#include "filelist.h"

const char *file_context_dir[] = {
    "active_file",
    "selected_files",
    "asset_library_ref",
    "selected_asset_files",
    "id",
    "selected_ids",
    nullptr,
};

int /*eContextResult*/ file_context(const bContext *C,
                                    const char *member,
                                    bContextDataResult *result)
{
  bScreen *screen = CTX_wm_screen(C);
  SpaceFile *sfile = CTX_wm_space_file(C);
  FileSelectParams *params = ED_fileselect_get_active_params(sfile);

  BLI_assert(!ED_area_is_global(CTX_wm_area(C)));

  if (CTX_data_dir(member)) {
    CTX_data_dir_set(result, file_context_dir);
    return CTX_RESULT_OK;
  }

  /* The following member checks return file-list data, check if that needs refreshing first. */
  if (file_main_region_needs_refresh_before_draw(sfile)) {
    return CTX_RESULT_NO_DATA;
  }

  if (CTX_data_equals(member, "active_file")) {
    FileDirEntry *file = filelist_file(sfile->files, params->active_file);
    if (file == nullptr) {
      return CTX_RESULT_NO_DATA;
    }

    CTX_data_pointer_set(result, &screen->id, &RNA_FileSelectEntry, file);
    return CTX_RESULT_OK;
  }
  if (CTX_data_equals(member, "selected_files")) {
    const int num_files_filtered = filelist_files_ensure(sfile->files);

    for (int file_index = 0; file_index < num_files_filtered; file_index++) {
      if (filelist_entry_is_selected(sfile->files, file_index)) {
        FileDirEntry *entry = filelist_file(sfile->files, file_index);
        CTX_data_list_add(result, &screen->id, &RNA_FileSelectEntry, entry);
      }
    }

    CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
    return CTX_RESULT_OK;
  }

  if (CTX_data_equals(member, "asset_library_ref")) {
    FileAssetSelectParams *asset_params = ED_fileselect_get_asset_params(sfile);
    if (!asset_params) {
      return CTX_RESULT_NO_DATA;
    }

    CTX_data_pointer_set(
        result, &screen->id, &RNA_AssetLibraryReference, &asset_params->asset_library_ref);
    return CTX_RESULT_OK;
  }
  /** TODO temporary AssetHandle design: For now this returns the file entry. Would be better if it
   * was `"selected_assets"` and returned the assets (e.g. as `AssetHandle`) directly. See comment
   * for #AssetHandle for more info. */
  if (CTX_data_equals(member, "selected_asset_files")) {
    const int num_files_filtered = filelist_files_ensure(sfile->files);

    for (int file_index = 0; file_index < num_files_filtered; file_index++) {
      if (filelist_entry_is_selected(sfile->files, file_index)) {
        FileDirEntry *entry = filelist_file(sfile->files, file_index);
        if (entry->asset) {
          CTX_data_list_add(result, &screen->id, &RNA_FileSelectEntry, entry);
        }
      }
    }

    CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
    return CTX_RESULT_OK;
  }
  if (CTX_data_equals(member, "id")) {
    const FileDirEntry *file = filelist_file(sfile->files, params->active_file);
    if (file == nullptr) {
      return CTX_RESULT_NO_DATA;
    }

    ID *id = filelist_file_get_id(file);
    if (id == nullptr) {
      return CTX_RESULT_NO_DATA;
    }

    CTX_data_id_pointer_set(result, id);
    return CTX_RESULT_OK;
  }
  if (CTX_data_equals(member, "selected_ids")) {
    const int num_files_filtered = filelist_files_ensure(sfile->files);

    for (int file_index = 0; file_index < num_files_filtered; file_index++) {
      if (!filelist_entry_is_selected(sfile->files, file_index)) {
        continue;
      }
      ID *id = filelist_entry_get_id(sfile->files, file_index);
      if (!id) {
        continue;
      }

      CTX_data_id_list_add(result, id);
    }

    CTX_data_type_set(result, CTX_DATA_TYPE_COLLECTION);
    return CTX_RESULT_OK;
  }

  return CTX_RESULT_MEMBER_NOT_FOUND;
}