target_sources(dosboxcommon PRIVATE
  autoexec.cpp
  command_line.cpp
  file_reader.cpp
  shell.cpp
  shell_batch.cpp
  shell_history.cpp
  shell_misc.cpp)

# Only the shell implementation needs the platform-specific networking shim.
# Keeping it out of the public shell header avoids pulling Winsock headers into
# unrelated translation units, where windows.h may already have included the
# legacy winsock.h.
set_source_files_properties(shell.cpp PROPERTIES
  COMPILE_DEFINITIONS DOSBOX_SHELL_NETWORK_COMPAT)

target_sources(dosbox PRIVATE shell_cmds.cpp)
