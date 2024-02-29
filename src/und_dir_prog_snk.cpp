#include "stdafx.hpp"
#include "data_types.hpp"
#include "imgui_specific.hpp"

HRESULT undelete_directory_progress_sink::StartOperations() { print_debug_msg("undelete_directory_progress_sink :: StartOperations"); return S_OK; }
HRESULT undelete_directory_progress_sink::PauseTimer()      { print_debug_msg("undelete_directory_progress_sink :: PauseTimer");      return S_OK; }
HRESULT undelete_directory_progress_sink::ResetTimer()      { print_debug_msg("undelete_directory_progress_sink :: ResetTimer");      return S_OK; }
HRESULT undelete_directory_progress_sink::ResumeTimer()     { print_debug_msg("undelete_directory_progress_sink :: ResumeTimer");     return S_OK; }

HRESULT undelete_directory_progress_sink::PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) { print_debug_msg("undelete_directory_progress_sink :: PostNewItem");    return S_OK; }
HRESULT undelete_directory_progress_sink::PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *)              { print_debug_msg("undelete_directory_progress_sink :: PostRenameItem"); return S_OK; }

HRESULT undelete_directory_progress_sink::PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) { print_debug_msg("undelete_directory_progress_sink :: PreCopyItem");   return S_OK; }
HRESULT undelete_directory_progress_sink::PreDeleteItem(DWORD, IShellItem *)                      { print_debug_msg("undelete_directory_progress_sink :: PreDeleteItem"); return S_OK; }
HRESULT undelete_directory_progress_sink::PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) { print_debug_msg("undelete_directory_progress_sink :: PreMoveItem");   return S_OK; }
HRESULT undelete_directory_progress_sink::PreNewItem(DWORD, IShellItem *, LPCWSTR)                { print_debug_msg("undelete_directory_progress_sink :: PreNewItem");    return S_OK; }
HRESULT undelete_directory_progress_sink::PreRenameItem(DWORD, IShellItem *, LPCWSTR)             { print_debug_msg("undelete_directory_progress_sink :: PreRenameItem"); return S_OK; }

HRESULT undelete_directory_progress_sink::PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) { print_debug_msg("undelete_directory_progress_sink :: PostMoveItem");   return S_OK; }
HRESULT undelete_directory_progress_sink::PostDeleteItem(DWORD, IShellItem *, HRESULT, IShellItem *)                      { print_debug_msg("undelete_directory_progress_sink :: PostDeleteItem"); return S_OK; }
HRESULT undelete_directory_progress_sink::PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) { print_debug_msg("undelete_directory_progress_sink :: PostCopyItem");   return S_OK; }

HRESULT undelete_directory_progress_sink::UpdateProgress(UINT work_total, UINT work_so_far)
{
    print_debug_msg("undelete_directory_progress_sink :: UpdateProgress %zu/%zu", work_so_far, work_total);
    return S_OK;
}

HRESULT undelete_directory_progress_sink::FinishOperations(HRESULT)
{
    print_debug_msg("undelete_directory_progress_sink :: FinishOperations");

    // path_force_separator(src_path_utf8, global_state::settings().dir_separator_utf8); //! UB
    path_force_separator(this->destination_full_path_utf8, global_state::settings().dir_separator_utf8); //! UB

    {
        auto pair = global_state::completed_file_ops();
        auto &completed_file_ops = *pair.first;
        auto &mutex = *pair.second;

        std::scoped_lock lock(mutex);

        auto found = std::find_if(completed_file_ops.begin(), completed_file_ops.end(),
                                  [&](completed_file_operation const &cfo) { return path_equals_exactly(cfo.src_path, this->destination_full_path_utf8); });

        if (found != completed_file_ops.end()) {
            found->undo_time = current_time_system();
            (void) global_state::save_completed_file_ops_to_disk(&lock);
        }
    }

    return S_OK;
}

ULONG undelete_directory_progress_sink::AddRef() { return 1; }
ULONG undelete_directory_progress_sink::Release() { return 1; }

HRESULT undelete_directory_progress_sink::QueryInterface(const IID &riid, void **ppv)
{
    print_debug_msg("undelete_directory_progress_sink :: QueryInterface");

    if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
        *ppv = static_cast<IFileOperationProgressSink*>(this);
        return S_OK;
    }

    *ppv = nullptr;
    return E_NOINTERFACE;
}
