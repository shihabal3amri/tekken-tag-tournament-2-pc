# cmake -DOUT=<file> -DF101=<path> ... -DF107=<path> -P hash_payload.cmake
# The installer's checksum list (payload_ids.h IDR_PAYLOAD_SHA256): one "<resource id> <sha256>" line per embedded file,
# so the installer can tell a damaged download (or a file damaged on its way to the disk) from a good one.
set(lines "")
foreach(id 101 102 103 104 105 106 107)
    if(NOT DEFINED F${id} OR NOT EXISTS "${F${id}}")
        message(FATAL_ERROR "hash_payload: no file for resource ${id}")
    endif()
    file(SHA256 "${F${id}}" h)
    string(APPEND lines "${id} ${h}\n")
endforeach()
file(WRITE "${OUT}" "${lines}")
