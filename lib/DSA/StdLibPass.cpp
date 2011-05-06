//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Recognize common standard c library functions and generate graphs for them
// FIXME: Move table to separate analysis pass, so that even the Local Pass
// may query it.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "dsa/DataStructure.h"
#include "dsa/AllocatorIdentification.h"
#include "dsa/DSGraph.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Timer.h"
#include <iostream>
#include "llvm/Module.h"

using namespace llvm;

static RegisterPass<StdLibDataStructures>
X("dsa-stdlib", "Standard Library Local Data Structure Analysis");

STATISTIC(NumNodesFoldedInStdLib,    "Number of nodes folded in std lib");

char StdLibDataStructures::ID;

#define numOps 10
namespace {
  static cl::opt<bool> noStdLibFold("dsa-stdlib-no-fold",
         cl::desc("Don't fold nodes in std-lib."),
         cl::Hidden,
         cl::init(false));
  static cl::opt<bool> DisableStdLib("disable-dsa-stdlib",
         cl::desc("Don't use DSA's stdlib pass."),
         cl::Hidden,
         cl::init(false));
}

//
// Structure: libAction
//
// Description:
//  Describe how the DSGraph of a function should be built.  Note that for the
//  boolean arrays of arity numOps, the first element is a flag describing the
//  return value, and the remaining elements are flags describing the
//  function's arguments.
//
struct libAction {
  // The return value/arguments that should be marked read.
  bool read[numOps];

  // The return value/arguments that should be marked modified.
  bool write[numOps];

  // The return value/arguments that should be marked as heap.
  bool heap[numOps];

  // Flags whether the return value should be merged with all arguments.
  bool mergeNodes[numOps];

  // Flags whether the return value and arguments should be folded.
  bool collapse;
};

#define NRET_NARGS    {0,0,0,0,0,0,0,0,0,0}
#define YRET_NARGS    {1,0,0,0,0,0,0,0,0,0}
#define NRET_YARGS    {0,1,1,1,1,1,1,1,1,1}
#define YRET_YARGS    {1,1,1,1,1,1,1,1,1,1}
#define NRET_NYARGS   {0,0,1,1,1,1,1,1,1,1}
#define YRET_NYARGS   {1,0,1,1,1,1,1,1,1,1}
#define NRET_YNARGS   {0,1,0,0,0,0,0,0,0,0}
#define YRET_YNARGS   {1,1,0,0,0,0,0,0,0,0}
#define YRET_NNYARGS  {1,0,0,1,1,1,1,1,1,1}
#define NRET_NNYARGS  {0,0,0,1,1,1,1,1,1,1}
#define YRET_NNYNARGS {1,0,0,1,0,0,0,0,0,0}
#define NRET_NNNYARGS {0,0,0,0,1,1,1,1,1,1}

const struct {
  const char* name;
  libAction action;
} recFuncs[] = {
  {"stat",       {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fstat",      {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, false}},
  {"lstat",      {NRET_YNARGS, NRET_NYARGS, NRET_NARGS, NRET_NARGS, false}},
  
  {"getenv",     {NRET_YNARGS, YRET_NARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"getrusage",  {NRET_YNARGS, YRET_NYARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"getrlimit",  {NRET_YNARGS, YRET_NYARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"setrlimit",  {NRET_YARGS,  YRET_NARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"getcwd",     {NRET_NYARGS, YRET_YNARGS, NRET_NARGS, YRET_YNARGS, false}},
  
  {"select",    {NRET_YARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  {"_setjmp",   {NRET_YARGS, YRET_YARGS,  NRET_NARGS, NRET_NARGS, false}},
  {"longjmp",   {NRET_YARGS, NRET_YARGS,  NRET_NARGS, NRET_NARGS, false}},
  
  {"remove",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"rename",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"unlink",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fileno",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"create",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"write",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"read",      {NRET_YARGS, YRET_YARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"truncate",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"open",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
 
  {"chdir",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"mkdir",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"rmdir",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  
  {"chmod",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fchmod",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
 
  {"kill",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"pipe",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  
  {"execl",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"execlp",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"execle",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"execv",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"execvp",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
 
  {"time",      {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"times",     {NRET_YARGS,  YRET_YARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"ctime",     {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"asctime",   {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"utime",     {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"localtime", {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"gmtime",    {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}}, 
  {"ftime",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, false}}, 

  // printf not strictly true, %n could cause a write
  {"printf",    {NRET_YARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS, false}},
  {"fprintf",   {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fprintf",   {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sprintf",   {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  {"snprintf",  {NRET_YARGS,  NRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  {"vsnprintf", {NRET_YARGS,  YRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sscanf",    {NRET_YARGS,  YRET_NYARGS, NRET_NARGS, NRET_NARGS, false}},
  {"scanf",     {NRET_YARGS,  YRET_NYARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fscanf",    {NRET_YARGS,  YRET_NYARGS, NRET_NARGS, NRET_NARGS, false}},

  {"calloc",    {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, false}},
  {"malloc",    {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, false}},
  {"valloc",    {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, false}},
  {"realloc",   {NRET_NARGS, YRET_NARGS, YRET_YNARGS, YRET_YNARGS,false}},
  {"free",      {NRET_NARGS, NRET_NARGS, NRET_YNARGS, NRET_NARGS, false}},
 
  {"strdup",    {NRET_YARGS, YRET_NARGS, YRET_NARGS, YRET_YARGS, false}},
  {"__strdup",  {NRET_YARGS, YRET_NARGS, YRET_NARGS, YRET_YARGS, false}},
  {"wcsdup",    {NRET_YARGS, YRET_NARGS, YRET_NARGS, YRET_YARGS, false}},
 
  {"strlen",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"wcslen",    {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
 
  {"atoi",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"atof",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"atol",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"atoll",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"atoq",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
 
  {"memcmp",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"strcmp",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"wcscmp",      {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"strncmp",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"wcsncmp",     {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"strcasecmp",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"wcscasecmp",  {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"strncasecmp", {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"wcsncasecmp", {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  
  {"strcat",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"strncat",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  
  {"strcpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"stpcpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"wcscpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"strncpy",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"wcsncpy",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"memcpy",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"memccpy",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"wmemccpy",   {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}},
  {"memmove",    {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YARGS, true}}, 
  
  {"bcopy",      {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_YARGS, true}},
  {"bcmp",       {NRET_YARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, true}},
  
  {"strerror",   {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  true}},
  {"clearerr",   {NRET_YARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"strstr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"wcsstr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"strspn",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  true}},
  {"wcsspn",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  true}},
  {"strcspn",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  true}},
  {"wcscspn",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS,  true}},
  {"strtok",     {NRET_YARGS, YRET_YARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"strpbrk",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"wcspbrk",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},

  {"strchr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"wcschr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"strrchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"wcsrchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"strchrnul",  {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"wcschrnul",  {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},

  {"memchr",     {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"wmemchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},
  {"memrchr",    {NRET_YARGS, YRET_NARGS, NRET_NARGS, YRET_YNARGS, true}},

  {"memalign",   {NRET_NARGS, YRET_NARGS, YRET_NARGS,  NRET_NARGS, false}},
  //{"posix_memalign",  {NRET_YARGS, YRET_YNARGS, NRET_NARGS,  NRET_NARGS, false}},

  {"perror",     {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  
  {"feof",       {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fflush",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fpurge",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fclose",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fopen",      {NRET_YARGS,  YRET_NARGS, YRET_NARGS, NRET_NARGS, false}},
  {"ftell",      {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fseek",      {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, true}},
  {"rewind",     {NRET_YARGS,  NRET_YARGS, NRET_NARGS, NRET_NARGS, true}},
  {"ferror",     {NRET_YARGS,  NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fwrite",     {NRET_YARGS,  NRET_NYARGS,NRET_NARGS, NRET_NARGS, false}},
  {"fread",      {NRET_NYARGS, NRET_YARGS, NRET_NARGS, NRET_NARGS, false}},
  {"fdopen",     {NRET_YARGS,  YRET_NARGS, NRET_NARGS, NRET_NARGS, false}},

  {"__errno_location", {NRET_NARGS, YRET_NARGS, NRET_NARGS, NRET_NARGS, false}},

  {"puts",       {NRET_YARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"gets",       {NRET_NARGS,  YRET_YARGS,  NRET_NARGS, YRET_YNARGS, false}},
  {"fgets",      {NRET_NYARGS, YRET_YNARGS, NRET_NARGS, YRET_YNARGS, false}},
  {"getc",       {NRET_YNARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"ungetc",     {NRET_YNARGS, YRET_YARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"_IO_getc",   {NRET_YNARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"fgetc",      {NRET_YNARGS, YRET_YNARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"putc",       {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"_IO_putc",   {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"putchar",    {NRET_NARGS,  NRET_NARGS,  NRET_NARGS, NRET_NARGS,  false}},
  {"fputs",      {NRET_YARGS,  NRET_NYARGS, NRET_NARGS, NRET_NARGS,  false}},
  {"fputc",      {NRET_YARGS,  NRET_NYARGS, NRET_NARGS, NRET_NARGS,  false}},

  
  // SAFECode Intrinsics
  {"sc.lscheck", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.lscheckui", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.lscheckalign", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.lscheckalignui", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_register_stack", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_unregister_stack", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_register_global", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_unregister_global", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_register", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_unregister", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  {"sc.pool_argvregister", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},

  // CStdLib Runtime Wrapper Functions
  {"pool_strncpy",    {NRET_NNYARGS, YRET_NNYARGS,  NRET_NARGS, YRET_NNYARGS,  true}},
  {"pool_strcpy",     {NRET_NNYARGS, YRET_NNYARGS,  NRET_NARGS, YRET_NNYARGS,  true}},
  {"pool_stpcpy",     {NRET_NNYARGS, YRET_NNYARGS,  NRET_NARGS, YRET_NNYARGS,  true}},
  // strchr and index have same functionality
  {"pool_strchr",     {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, YRET_NYARGS,   true}},
  {"pool_index",      {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, YRET_NYARGS,   true}},
  // strrchr and rindex have same functionality
  {"pool_strrchr",    {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, YRET_NYARGS,   true}},
  {"pool_rindex",     {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, YRET_NYARGS,   true}},
  {"pool_strcat",     {NRET_NNYARGS, YRET_NNYARGS,  NRET_NARGS, YRET_NNYARGS,  true}},
  {"pool_strncat",    {NRET_NNYARGS, YRET_NNYARGS,  NRET_NARGS, YRET_NNYARGS,  true}},
  {"pool_strstr",     {NRET_NNYARGS, YRET_NARGS,    NRET_NARGS, YRET_NNYNARGS, true}},
  {"pool_strcasestr", {NRET_NNYARGS, YRET_NARGS,    NRET_NARGS, YRET_NNYNARGS, true}},
  {"pool_strpbrk",    {NRET_NNYARGS, YRET_NARGS,    NRET_NARGS, YRET_NNYNARGS, true}},
  {"pool_strspn",     {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, NRET_NARGS,    true}},
  {"pool_strcspn",    {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, NRET_NARGS,    true}},
  {"pool_memccpy",    {NRET_NNYARGS, YRET_NNYARGS,  NRET_NARGS, YRET_NNYARGS,  true}},
  {"pool_memchr",     {NRET_NYARGS,  YRET_NARGS,    NRET_NARGS, YRET_NYARGS,   true}},
  {"pool_strcmp",     {NRET_NNYARGS, NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_strncmp",    {NRET_NNYARGS, NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_strlen",     {NRET_NYARGS,  NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_strnlen",    {NRET_NYARGS,  NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_memcmp",     {NRET_NNYARGS, NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_strcasecmp", {NRET_NNYARGS, NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_strncasecmp",{NRET_NNYARGS, NRET_NARGS,    NRET_NARGS, NRET_NARGS,   false}},
  {"pool_bcopy",      {NRET_NNYARGS, NRET_NNNYARGS, NRET_NARGS, NRET_NNYARGS,  true}},
  {"pool_bcmp",       {NRET_NNYARGS, NRET_NARGS,    NRET_NARGS, NRET_NARGS,    true}},

#if 0
  {"wait",       {false, false, false, false,  true, false, false, false, false}},
#endif

  // C++ functions, as mangled on linux gcc 4.2
  // operator new(unsigned long)
  {"_Znwm",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, false}},
  // operator new[](unsigned long)
  {"_Znam",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, false}},
  // operator new(unsigned int)
  {"_Znwj",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, false}},
  // operator new[](unsigned int)
  {"_Znaj",      {NRET_NARGS, YRET_NARGS, YRET_NARGS, NRET_NARGS, false}},
  // operator delete(void*)
  {"_ZdlPv",     {NRET_NARGS, NRET_NARGS, NRET_YNARGS,NRET_NARGS, false}},
  // operator delete[](void*)
  {"_ZdaPv",     {NRET_NARGS, NRET_NARGS, NRET_YNARGS, NRET_NARGS, false}},
  // flush
  {"_ZNSo5flushEv", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  // << operator
  {"_ZNSolsEd", {NRET_YARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  // << operator
  {"_ZNSolsEPFRSoS_E", {NRET_YARGS, NRET_YNARGS, NRET_NARGS, NRET_NARGS, false}},
  //endl
  {"_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_", {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
  // Terminate the list of special functions recognized by this pass
  {0,            {NRET_NARGS, NRET_NARGS, NRET_NARGS, NRET_NARGS, false}},
};

/*
   Functions to add
   freopen
   strftime
   strtoul
   strtol
   strtoll
   ctype family
   setbuf
   setvbuf
   __strpbrk_c3
   open64/fopen64/lseek64
 */

//
// Method: eraseCallsTo()
//
// Description:
//  This method removes the specified function from DSCallsites within the
//  specified function.  We do not do anything with call sites that call this
//  function indirectly (for which there is not much point as we do not yet
//  know the targets of indirect function calls).
//
void
StdLibDataStructures::eraseCallsTo(Function* F) {
  for (Value::use_iterator ii = F->use_begin(), ee = F->use_end();
       ii != ee; ++ii)
    if (CallInst* CI = dyn_cast<CallInst>(ii)){
      if (CI->getOperand(0) == F) {
        DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());
        //delete the call
        DEBUG(errs() << "Removing " << F->getNameStr() << " from " 
	      << CI->getParent()->getParent()->getNameStr() << "\n");
        Graph->removeFunctionCalls(*F);
      }
    }else if (InvokeInst* CI = dyn_cast<InvokeInst>(ii)){
      if (CI->getOperand(0) == F) {
        DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());
        //delete the call
        DEBUG(errs() << "Removing " << F->getNameStr() << " from " 
	      << CI->getParent()->getParent()->getNameStr() << "\n");
        Graph->removeFunctionCalls(*F);
      }
    } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(ii)) {
      if(CE->isCast()) {
        for (Value::use_iterator ci = CE->use_begin(), ce = CE->use_end();
             ci != ce; ++ci) {
          if (CallInst* CI = dyn_cast<CallInst>(ci)){
            if(CI->getOperand(0) == CE) {
              DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());
              //delete the call
              DEBUG(errs() << "Removing " << F->getNameStr() << " from " 
	        << CI->getParent()->getParent()->getNameStr() << "\n");
              Graph->removeFunctionCalls(*F);
            }
          }
        }
      }
    }
}

//
// Function: processRuntimeCheck()
//
// Description:
//  Modify a run-time check so that its return value has the same DSNode as the
//  checked pointer.
//
// Inputs:
//  M    - The module in which calls to the function live.
//  name - The name of the function for which direct calls should be processed.
//  arg  - The argument index that contains the pointer which the run-time
//         check returns.
//
void
StdLibDataStructures::processRuntimeCheck (Module & M,
                                           std::string name,
                                           unsigned arg) {
  //
  // Get a pointer to the function.
  //
  Function* F = M.getFunction (name);

  //
  // If the function doesn't exist, then there is no work to do.
  //
  if (!F) return;

  //
  // Scan through all direct calls to the function (there should only be direct
  // calls) and process each one.
  //
  for (Value::use_iterator ii = F->use_begin(), ee = F->use_end();
       ii != ee; ++ii) {
    if (CallInst* CI = dyn_cast<CallInst>(ii)) {
      if (CI->getOperand(0) == F) {
        DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());
        DSNodeHandle RetNode = Graph->getNodeForValue(CI);
        DSNodeHandle ArgNode = Graph->getNodeForValue(CI->getOperand(arg));
        RetNode.mergeWith(ArgNode);
      }
    }
  }

  //
  // Erase the DSCallSites for this function.  This should prevent other DSA
  // passes from making the DSNodes passed to/returned from the function
  // from becoming Incomplete or External.
  //
  eraseCallsTo (F);
  return;
}

bool
StdLibDataStructures::runOnModule (Module &M) {
  //
  // Get the results from the local pass.
  //
  init (&getAnalysis<LocalDataStructures>(), true, true, false, false);
  AllocWrappersAnalysis = &getAnalysis<AllocIdentify>();

  //
  // Fetch the DSGraphs for all defined functions within the module.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) 
    if (!I->isDeclaration())
      getOrCreateGraph(&*I);

  //
  // Erase direct calls to functions that don't return a pointer and are marked
  // with the readnone annotation.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) 
    if (I->isDeclaration() && I->doesNotAccessMemory() &&
        !isa<PointerType>(I->getReturnType()))
      eraseCallsTo(I);

  //
  // Erase direct calls to external functions that are not varargs, do not
  // return a pointer, and do not take pointers.
  //
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) 
    if (I->isDeclaration() && !I->isVarArg() &&
        !isa<PointerType>(I->getReturnType())) {
      bool hasPtr = false;
      for (Function::arg_iterator ii = I->arg_begin(), ee = I->arg_end();
           ii != ee;
           ++ii)
        if (isa<PointerType>(ii->getType())) {
          hasPtr = true;
          break;
        }
      if (!hasPtr)
        eraseCallsTo(I);
    }

  if(!DisableStdLib) {

    //
    // Scan through the function summaries and process functions by summary.
    //
    for (int x = 0; recFuncs[x].name; ++x) 
      if (Function* F = M.getFunction(recFuncs[x].name))
        if (F->isDeclaration()) {
          processFunction(x, F);
        }

    std::set<std::string>::iterator ai = AllocWrappersAnalysis->alloc_begin();
    std::set<std::string>::iterator ae = AllocWrappersAnalysis->alloc_end();
    int x;
    for (x = 0; recFuncs[x].name; ++x) {
      if(recFuncs[x].name == std::string("malloc"))
        break;
    }

    for(;ai != ae; ++ai) {
      if(Function* F = M.getFunction(*ai))
        processFunction(x, F);
    }

    ai = AllocWrappersAnalysis->dealloc_begin();
    ae = AllocWrappersAnalysis->dealloc_end();
    for (x = 0; recFuncs[x].name; ++x) {
      if(recFuncs[x].name == std::string("free"))
        break;
    }

    for(;ai != ae; ++ai) {
      if(Function* F = M.getFunction(*ai))
        processFunction(x, F);
    }

    //
    // Merge return values and checked pointer values for SAFECode run-time
    // checks.
    //
    processRuntimeCheck (M, "sc.boundscheck", 3);
    processRuntimeCheck (M, "sc.boundscheckui", 3);
    processRuntimeCheck (M, "sc.exactcheck2", 2);
    processRuntimeCheck (M, "sc.get_actual_val", 2);
  }

  //
  // In the Local DSA Pass, we marked nodes passed to/returned from 'StdLib'
  // functions as External because, at that point, they were.  However, they no
  // longer are necessarily External, and we need to update accordingly.
  //
  GlobalsGraph->computeExternalFlags(DSGraph::ResetExternal);
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isDeclaration()) {
      DSGraph * G = getDSGraph(*I);
      unsigned EFlags = 0
        | DSGraph::ResetExternal
        | DSGraph::DontMarkFormalsExternal
        | DSGraph::ProcessCallSites;
      G->computeExternalFlags(EFlags);
      DEBUG(G->AssertGraphOK());
    }
  GlobalsGraph->computeExternalFlags(DSGraph::ProcessCallSites);
  DEBUG(GlobalsGraph->AssertGraphOK());

  return false;
}


void StdLibDataStructures::processFunction(int x, Function *F) {
  for (Value::use_iterator ii = F->use_begin(), ee = F->use_end();
       ii != ee; ++ii)
    if (CallInst* CI = dyn_cast<CallInst>(ii)){
      if (CI->getOperand(0) == F) {
        DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());

        //
        // Set the read, write, and heap markers on the return value
        // as appropriate.
        //
        if(isa<PointerType>((CI)->getType())){
          if(Graph->hasNodeForValue(CI)){
            if (recFuncs[x].action.read[0])
              Graph->getNodeForValue(CI).getNode()->setReadMarker();
            if (recFuncs[x].action.write[0])
              Graph->getNodeForValue(CI).getNode()->setModifiedMarker();
            if (recFuncs[x].action.heap[0])
              Graph->getNodeForValue(CI).getNode()->setHeapMarker();
          }
        }

        //
        // Set the read, write, and heap markers on the actual arguments
        // as appropriate.
        //
        for (unsigned y = 1; y < CI->getNumOperands(); ++y)
          if (isa<PointerType>(CI->getOperand(y)->getType())){
            if (Graph->hasNodeForValue(CI->getOperand(y))){
              if (recFuncs[x].action.read[y])
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->setReadMarker();
              if (recFuncs[x].action.write[y])
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->setModifiedMarker();
              if (recFuncs[x].action.heap[y])
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->setHeapMarker();
            }
          }

        //
        // Merge the DSNoes for return values and parameters as
        // appropriate.
        //
        std::vector<DSNodeHandle> toMerge;
        if (recFuncs[x].action.mergeNodes[0])
          if (isa<PointerType>(CI->getType()))
            if (Graph->hasNodeForValue(CI))
              toMerge.push_back(Graph->getNodeForValue(CI));
        for (unsigned y = 1; y < CI->getNumOperands(); ++y)
          if (recFuncs[x].action.mergeNodes[y])
            if (isa<PointerType>(CI->getOperand(y)->getType()))
              if (Graph->hasNodeForValue(CI->getOperand(y)))
                toMerge.push_back(Graph->getNodeForValue(CI->getOperand(y)));
        for (unsigned y = 1; y < toMerge.size(); ++y)
          toMerge[0].mergeWith(toMerge[y]);

        //
        // Collapse (fold) the DSNode of the return value and the actual
        // arguments if directed to do so.
        //
        if (!noStdLibFold && recFuncs[x].action.collapse) {
          if (isa<PointerType>(CI->getType())){
            if (Graph->hasNodeForValue(CI))
              Graph->getNodeForValue(CI).getNode()->foldNodeCompletely();
            NumNodesFoldedInStdLib++;
          }
          for (unsigned y = 1; y < CI->getNumOperands(); ++y){
            if (isa<PointerType>(CI->getOperand(y)->getType())){
              if (Graph->hasNodeForValue(CI->getOperand(y))){
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->foldNodeCompletely();
                NumNodesFoldedInStdLib++;
              }
            }
          }
        }
      }
    } else if (InvokeInst* CI = dyn_cast<InvokeInst>(ii)){
      if (CI->getOperand(0) == F) {
        DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());

        //
        // Set the read, write, and heap markers on the return value
        // as appropriate.
        //
        if(isa<PointerType>((CI)->getType())){
          if(Graph->hasNodeForValue(CI)){
            if (recFuncs[x].action.read[0])
              Graph->getNodeForValue(CI).getNode()->setReadMarker();
            if (recFuncs[x].action.write[0])
              Graph->getNodeForValue(CI).getNode()->setModifiedMarker();
            if (recFuncs[x].action.heap[0])
              Graph->getNodeForValue(CI).getNode()->setHeapMarker();
          }
        }

        //
        // Set the read, write, and heap markers on the actual arguments
        // as appropriate.
        //
        for (unsigned y = 1; y < CI->getNumOperands(); ++y)
          if (isa<PointerType>(CI->getOperand(y)->getType())){
            if (Graph->hasNodeForValue(CI->getOperand(y))){
              if (recFuncs[x].action.read[y])
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->setReadMarker();
              if (recFuncs[x].action.write[y])
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->setModifiedMarker();
              if (recFuncs[x].action.heap[y])
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->setHeapMarker();
            }
          }

        //
        // Merge the DSNoes for return values and parameters as
        // appropriate.
        //
        std::vector<DSNodeHandle> toMerge;
        if (recFuncs[x].action.mergeNodes[0])
          if (isa<PointerType>(CI->getType()))
            if (Graph->hasNodeForValue(CI))
              toMerge.push_back(Graph->getNodeForValue(CI));
        for (unsigned y = 1; y < CI->getNumOperands(); ++y)
          if (recFuncs[x].action.mergeNodes[y])
            if (isa<PointerType>(CI->getOperand(y)->getType()))
              if (Graph->hasNodeForValue(CI->getOperand(y)))
                toMerge.push_back(Graph->getNodeForValue(CI->getOperand(y)));
        for (unsigned y = 1; y < toMerge.size(); ++y)
          toMerge[0].mergeWith(toMerge[y]);

        //
        // Collapse (fold) the DSNode of the return value and the actual
        // arguments if directed to do so.
        //
        if (!noStdLibFold && recFuncs[x].action.collapse) {
          if (isa<PointerType>(CI->getType())){
            if (Graph->hasNodeForValue(CI))
              Graph->getNodeForValue(CI).getNode()->foldNodeCompletely();
            NumNodesFoldedInStdLib++;
          }
          for (unsigned y = 1; y < CI->getNumOperands(); ++y){
            if (isa<PointerType>(CI->getOperand(y)->getType())){
              if (Graph->hasNodeForValue(CI->getOperand(y))){
                Graph->getNodeForValue(CI->getOperand(y)).getNode()->foldNodeCompletely();
                NumNodesFoldedInStdLib++;
              }
            }
          }
        }
      }
    } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(ii)) {
      if(CE->isCast()) 
        for (Value::use_iterator ci = CE->use_begin(), ce = CE->use_end();
             ci != ce; ++ci) {

          if (CallInst* CI = dyn_cast<CallInst>(ci)){
            if (CI->getOperand(0) == CE) {
              DSGraph* Graph = getDSGraph(*CI->getParent()->getParent());

              //
              // Set the read, write, and heap markers on the return value
              // as appropriate.
              //
              if(isa<PointerType>((CI)->getType())){
                if(Graph->hasNodeForValue(CI)){
                  if (recFuncs[x].action.read[0])
                    Graph->getNodeForValue(CI).getNode()->setReadMarker();
                  if (recFuncs[x].action.write[0])
                    Graph->getNodeForValue(CI).getNode()->setModifiedMarker();
                  if (recFuncs[x].action.heap[0])
                    Graph->getNodeForValue(CI).getNode()->setHeapMarker();
                }
              }

              //
              // Set the read, write, and heap markers on the actual arguments
              // as appropriate.
              //
              for (unsigned y = 1; y < CI->getNumOperands(); ++y)
                if (recFuncs[x].action.read[y]){
                  if (isa<PointerType>(CI->getOperand(y)->getType())){
                    if (Graph->hasNodeForValue(CI->getOperand(y)))
                      Graph->getNodeForValue(CI->getOperand(y)).getNode()->setReadMarker();
                    if (Graph->hasNodeForValue(CI->getOperand(y)))
                      Graph->getNodeForValue(CI->getOperand(y)).getNode()->setModifiedMarker();
                    if (Graph->hasNodeForValue(CI->getOperand(y)))
                      Graph->getNodeForValue(CI->getOperand(y)).getNode()->setHeapMarker();
                  }
                }

              //
              // Merge the DSNoes for return values and parameters as
              // appropriate.
              //
              std::vector<DSNodeHandle> toMerge;
              if (recFuncs[x].action.mergeNodes[0])
                if (isa<PointerType>(CI->getType()))
                  if (Graph->hasNodeForValue(CI))
                    toMerge.push_back(Graph->getNodeForValue(CI));
              for (unsigned y = 1; y < CI->getNumOperands(); ++y)
                if (recFuncs[x].action.mergeNodes[y])
                  if (isa<PointerType>(CI->getOperand(y)->getType()))
                    if (Graph->hasNodeForValue(CI->getOperand(y)))
                      toMerge.push_back(Graph->getNodeForValue(CI->getOperand(y)));
              for (unsigned y = 1; y < toMerge.size(); ++y)
                toMerge[0].mergeWith(toMerge[y]);

              //
              // Collapse (fold) the DSNode of the return value and the actual
              // arguments if directed to do so.
              //
              if (!noStdLibFold && recFuncs[x].action.collapse) {
                if (isa<PointerType>(CI->getType())){
                  if (Graph->hasNodeForValue(CI))
                    Graph->getNodeForValue(CI).getNode()->foldNodeCompletely();
                  NumNodesFoldedInStdLib++;
                }
                for (unsigned y = 1; y < CI->getNumOperands(); ++y)
                  if (isa<PointerType>(CI->getOperand(y)->getType())){
                    if (Graph->hasNodeForValue(CI->getOperand(y))){
                      DSNode * Node=Graph->getNodeForValue(CI->getOperand(y)).getNode();
                      Node->foldNodeCompletely();
                      NumNodesFoldedInStdLib++;
                    }
                  }
              }
            }
          }
        }
    }

  //
  // Pretend that this call site does not call this function anymore.
  //
  eraseCallsTo(F);
}
