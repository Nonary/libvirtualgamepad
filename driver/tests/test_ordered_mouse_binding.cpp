// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Compile the shipping CM/interface/status discovery body against inert APIs.
// These synthetic identifiers are fixtures, never accepted production IDs.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <vector>
#include "libvirtualgamepad/mouse_mapper_control.h"
#include "libvirtualgamepad/mouse_transport.h"
using GUID = lvg::mouse_mapper::guid;
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using ULONGLONG = std::uint64_t;
using DEVINST = std::uint32_t;
using DEVPROPTYPE = std::uint32_t;
using PBYTE = unsigned char *;
using PWSTR = wchar_t *;
using HANDLE = void *;
using BOOL = int;
constexpr int CR_SUCCESS=0, CR_FAILURE=1;
constexpr ULONG CM_GETIDLIST_FILTER_ENUMERATOR=1, CM_GETIDLIST_FILTER_PRESENT=2;
constexpr ULONG CM_GET_DEVICE_INTERFACE_LIST_PRESENT=0, CM_LOCATE_DEVNODE_NORMAL=0;
constexpr int DEVPKEY_Device_ContainerId=1, DEVPKEY_Device_HardwareIds=2;
constexpr DEVPROPTYPE DEVPROP_TYPE_GUID=1, DEVPROP_TYPE_STRING_LIST=2;
constexpr DWORD GENERIC_READ=0x80000000, FILE_SHARE_READ=1, FILE_SHARE_WRITE=2, OPEN_EXISTING=3;
#define LVG_KM_VENDOR_ID 65535
#define LVG_KM_PRODUCT_ID 65534
#define _countof(x) (sizeof(x)/sizeof((x)[0]))
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(std::intptr_t{-1})
namespace {
int checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"%d: %s\n",__LINE__,#x); } } while(false)
namespace mapper = lvg::mouse_mapper;
struct node {
  std::wstring id=L"HID\\Fixture\\One";
  GUID container=mapper::k_container_guid;
  GUID child_container=mapper::k_container_guid;
  std::wstring hardware=L"HID\\VID_FFFF&PID_FFFE&REV_0100&Col02";
  mapper::status status {sizeof(mapper::status), mapper::k_status_version,0,mapper::k_ready,mapper::k_container_guid};
  DWORD received=sizeof(mapper::status);
  ULONGLONG ready_at=0;
  bool interface=true, open=true, query=true, property=true, child=true;
};
std::vector<node> nodes;
ULONGLONG ticks=0;
unsigned opens=0,queries=0,closes=0;
bool bad_list=false;
template<class... Args> int swprintf_s(wchar_t *buffer,std::size_t count,const wchar_t *format,Args...args) {
  return std::swprintf(buffer,count,format,args...);
}
int _wcsicmp(const wchar_t *a,const wchar_t *b) {
  while (*a && *b && std::towlower(*a)==std::towlower(*b)) {++a;++b;}
  return static_cast<int>(std::towlower(*a))-static_cast<int>(std::towlower(*b));
}
ULONGLONG GetTickCount64() {return ticks;}
void Sleep(DWORD duration) {ticks+=duration;}
std::vector<wchar_t> multi(const std::vector<std::wstring> &values) {
  std::vector<wchar_t> output;
  for(const auto &value:values) {output.insert(output.end(),value.begin(),value.end());output.push_back(0);}
  output.push_back(0);
  if(output.size()==1) output.push_back(0);
  return output;
}
std::vector<wchar_t> device_ids() {
  std::vector<std::wstring> ids;
  for(const auto &entry:nodes) ids.push_back(entry.id);
  return multi(ids);
}
int CM_Get_Device_ID_List_SizeW(ULONG *count,const wchar_t *,ULONG) {
  *count=static_cast<ULONG>(device_ids().size());return CR_SUCCESS;
}
int CM_Get_Device_ID_ListW(const wchar_t *,wchar_t *buffer,ULONG count,ULONG) {
  const auto list=device_ids(); if(count<list.size()) return CR_FAILURE;
  std::copy(list.begin(),list.end(),buffer);
  if(bad_list) buffer[count-1]=L'X';
  return CR_SUCCESS;
}
int CM_Locate_DevNodeW(DEVINST *dev,PWSTR id,ULONG) {
  for(std::size_t i=0;i<nodes.size();++i) if(nodes[i].id==id) {*dev=static_cast<DEVINST>(i+1);return CR_SUCCESS;}
  return CR_FAILURE;
}
int CM_Get_DevNode_PropertyW(DEVINST dev,const int *key,DEVPROPTYPE *type,PBYTE bytes,ULONG *count,ULONG) {
  const bool child=dev>=1000;
  auto &entry=nodes.at(dev-(child?1000:0)-1);if(!entry.property) return CR_FAILURE;
  if(*key==DEVPKEY_Device_ContainerId) {
    if(*count<sizeof(GUID)) return CR_FAILURE;
    std::memcpy(bytes,child?&entry.child_container:&entry.container,sizeof(GUID));*count=sizeof(GUID);*type=DEVPROP_TYPE_GUID;return CR_SUCCESS;
  }
  const auto value=multi({entry.hardware});const auto size=value.size()*sizeof(wchar_t);
  if(*count<size) return CR_FAILURE;
  std::memcpy(bytes,value.data(),size);*count=static_cast<ULONG>(size);*type=DEVPROP_TYPE_STRING_LIST;return CR_SUCCESS;
}
std::wstring child_id(std::size_t index) {return L"VIBESHINE\\ORDERED_MOUSE_STATUS\\"+std::to_wstring(index);}
int CM_Get_Child(DEVINST *child,DEVINST parent,ULONG) {
  if(parent>=1000 || !nodes.at(parent-1).child)return CR_FAILURE;
  *child=parent+1000;return CR_SUCCESS;
}
int CM_Get_Sibling(DEVINST *,DEVINST,ULONG) {return CR_FAILURE;}
int CM_Get_Device_IDW(DEVINST dev,wchar_t *buffer,ULONG count,ULONG) {
  const auto id=child_id(dev-1001);if(count<=id.size())return CR_FAILURE;
  std::copy(id.begin(),id.end(),buffer);buffer[id.size()]=0;return CR_SUCCESS;
}
node *by_id(const wchar_t *id) {for(std::size_t i=0;i<nodes.size();++i) if(child_id(i)==id)return &nodes[i];return nullptr;}
int CM_Get_Device_Interface_List_SizeW(ULONG *count,const GUID *guid,PWSTR id,ULONG) {
  CHECK(mapper::same_guid(*guid,mapper::k_interface_guid));
  auto *entry=by_id(id);if(!entry)return CR_FAILURE;
  *count=static_cast<ULONG>(multi(entry->interface ? std::vector<std::wstring>{id}:std::vector<std::wstring>{}).size());return CR_SUCCESS;
}
int CM_Get_Device_Interface_ListW(const GUID *,PWSTR id,wchar_t *buffer,ULONG count,ULONG) {
  auto *entry=by_id(id);if(!entry)return CR_FAILURE;
  const auto list=multi(entry->interface? std::vector<std::wstring>{id}:std::vector<std::wstring>{});
  if(count<list.size())return CR_FAILURE;
  std::copy(list.begin(),list.end(),buffer);return CR_SUCCESS;
}
HANDLE CreateFileW(const wchar_t *path,DWORD access,DWORD share,void *,DWORD disposition,DWORD flags,void *) {
  CHECK(access==GENERIC_READ && share==(FILE_SHARE_READ|FILE_SHARE_WRITE));
  CHECK(disposition==OPEN_EXISTING && flags==0);++opens;
  auto *entry=by_id(path);return entry && entry->open?entry:INVALID_HANDLE_VALUE;
}
BOOL DeviceIoControl(HANDLE handle,DWORD code,void *input,DWORD input_size,void *output,DWORD output_size,DWORD *received,void *) {
  ++queries;CHECK(code==mapper::k_ioctl_status && input==nullptr && input_size==0);
  CHECK(output_size==sizeof(mapper::status));auto &entry=*static_cast<node *>(handle);
  auto status=entry.status;if(ticks<entry.ready_at)status.ready_flags=mapper::k_d0;
  std::memcpy(output,&status,sizeof(status));*received=entry.received;return entry.query;
}
void CloseHandle(HANDLE) {++closes;}
#include "../src/ordered_mouse_binding.inc"
void reset() {nodes={node{}};ticks=0;opens=queries=closes=0;bad_list=false;}
bool bind(DWORD timeout=0) {return wait_for_ordered_mouse_mapper(mapper::k_container_guid,timeout);}
}
int main() {
  reset();CHECK(bind());CHECK(opens==1 && queries==1 && closes==1);
  reset();nodes[0].hardware=L"hid\\vid_ffff&pid_fffe&rev_0100&col02";CHECK(bind());
  reset();nodes.clear();CHECK(!bind(25));CHECK(ticks==25 && opens==0);
  reset();nodes[0].container.data4[7]^=1;CHECK(!bind());CHECK(opens==0);
  reset();nodes[0].hardware=L"HID\\VID_FFFF&PID_FFFE&REV_0100&Col01";CHECK(!bind());CHECK(opens==0);
  reset();nodes[0].hardware=L"HID\\VID_FFFF&PID_FFFE&REV_0001&Col02";CHECK(!bind());
  reset();nodes[0].hardware=L"HID_DEVICE_UP:FF53_U:0001";CHECK(!bind());
  reset();nodes[0].hardware=L"HID\\VID_FFFF&PID_FFFD&REV_0100&Col02";CHECK(!bind());
  reset();nodes[0].property=false;CHECK(!bind());
  reset();nodes[0].child=false;CHECK(!bind());CHECK(opens==0);
  reset();nodes[0].child_container.data1++;CHECK(!bind());CHECK(opens==0);
  reset();nodes[0].interface=false;CHECK(!bind());CHECK(opens==0);
  reset();nodes[0].open=false;CHECK(!bind());CHECK(queries==0 && closes==0);
  reset();nodes[0].query=false;CHECK(!bind());CHECK(closes==1);
  reset();nodes[0].status.version++;CHECK(!bind());
  reset();nodes[0].status.size--;CHECK(!bind());
  reset();nodes[0].status.reserved=1;CHECK(!bind());
  reset();nodes[0].received--;CHECK(!bind());
  reset();nodes[0].status.container_id.data1++;CHECK(!bind());
  for(unsigned bit=1;bit<=4;bit<<=1) {reset();nodes[0].status.ready_flags&=~bit;CHECK(!bind());}
  reset();nodes[0].status.ready_flags|=8;CHECK(!bind());
  reset();nodes[0].ready_at=30;CHECK(bind(40));CHECK(ticks==30);
  reset();nodes[0].ready_at=50;CHECK(!bind(40));CHECK(ticks==40);
  reset();nodes[0].ready_at=6000;CHECK(!bind(10000));CHECK(ticks==5000);
  reset();nodes[0].container.data1++;nodes.push_back(node{});nodes[1].id=L"HID\\Fixture\\Two";
  CHECK(bind());CHECK(opens==1);
  reset();bad_list=true;CHECK(!bind());CHECK(opens==0);
  std::printf("ordered mouse binding: %d/%d checks passed\n",checks-failures,checks);
  return failures?1:0;
}
