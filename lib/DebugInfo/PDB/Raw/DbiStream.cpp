//===- DbiStream.cpp - PDB Dbi Stream (Stream 3) Access -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Raw/DbiStream.h"

#include "llvm/DebugInfo/CodeView/StreamArray.h"
#include "llvm/DebugInfo/CodeView/StreamReader.h"
#include "llvm/DebugInfo/PDB/Raw/ISectionContribVisitor.h"
#include "llvm/DebugInfo/PDB/Raw/IndexedStreamData.h"
#include "llvm/DebugInfo/PDB/Raw/InfoStream.h"
#include "llvm/DebugInfo/PDB/Raw/ModInfo.h"
#include "llvm/DebugInfo/PDB/Raw/NameHashTable.h"
#include "llvm/DebugInfo/PDB/Raw/PDBFile.h"
#include "llvm/DebugInfo/PDB/Raw/RawConstants.h"
#include "llvm/DebugInfo/PDB/Raw/RawError.h"
#include "llvm/DebugInfo/PDB/Raw/RawTypes.h"
#include "llvm/Object/COFF.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::pdb;
using namespace llvm::support;

namespace {
// Some of the values are stored in bitfields.  Since this needs to be portable
// across compilers and architectures (big / little endian in particular) we
// can't use the actual structures below, but must instead do the shifting
// and masking ourselves.  The struct definitions are provided for reference.

// struct DbiFlags {
//  uint16_t IncrementalLinking : 1;  // True if linked incrementally
//  uint16_t IsStripped : 1;          // True if private symbols were stripped.
//  uint16_t HasCTypes : 1;           // True if linked with /debug:ctypes.
//  uint16_t Reserved : 13;
//};
const uint16_t FlagIncrementalMask = 0x0001;
const uint16_t FlagStrippedMask = 0x0002;
const uint16_t FlagHasCTypesMask = 0x0004;

// struct DbiBuildNo {
//  uint16_t MinorVersion : 8;
//  uint16_t MajorVersion : 7;
//  uint16_t NewVersionFormat : 1;
//};
const uint16_t BuildMinorMask = 0x00FF;
const uint16_t BuildMinorShift = 0;

const uint16_t BuildMajorMask = 0x7F00;
const uint16_t BuildMajorShift = 8;
}

struct DbiStream::HeaderInfo {
  little32_t VersionSignature;
  ulittle32_t VersionHeader;
  ulittle32_t Age;                     // Should match InfoStream.
  ulittle16_t GlobalSymbolStreamIndex; // Global symbol stream #
  ulittle16_t BuildNumber;             // See DbiBuildNo structure.
  ulittle16_t PublicSymbolStreamIndex; // Public symbols stream #
  ulittle16_t PdbDllVersion;           // version of mspdbNNN.dll
  ulittle16_t SymRecordStreamIndex;    // Symbol records stream #
  ulittle16_t PdbDllRbld;              // rbld number of mspdbNNN.dll
  little32_t ModiSubstreamSize;        // Size of module info stream
  little32_t SecContrSubstreamSize;    // Size of sec. contribution stream
  little32_t SectionMapSize;           // Size of sec. map substream
  little32_t FileInfoSize;             // Size of file info substream
  little32_t TypeServerSize;      // Size of type server map
  ulittle32_t MFCTypeServerIndex; // Index of MFC Type Server
  little32_t OptionalDbgHdrSize;  // Size of DbgHeader info
  little32_t ECSubstreamSize;     // Size of EC stream (what is EC?)
  ulittle16_t Flags;              // See DbiFlags enum.
  ulittle16_t MachineType;        // See PDB_MachineType enum.

  ulittle32_t Reserved; // Pad to 64 bytes
};

template <typename ContribType>
Error loadSectionContribs(FixedStreamArray<ContribType> &Output,
                          StreamReader &Reader) {
  if (Reader.bytesRemaining() % sizeof(ContribType) != 0)
    return make_error<RawError>(
        raw_error_code::corrupt_file,
        "Invalid number of bytes of section contributions");

  uint32_t Count = Reader.bytesRemaining() / sizeof(ContribType);
  if (auto EC = Reader.readArray(Output, Count))
    return EC;
  return Error::success();
}

DbiStream::DbiStream(PDBFile &File, std::unique_ptr<MappedBlockStream> Stream)
    : Pdb(File), Stream(std::move(Stream)), Header(nullptr) {
  static_assert(sizeof(HeaderInfo) == 64, "Invalid HeaderInfo size!");
}

DbiStream::~DbiStream() {}

Error DbiStream::reload() {
  StreamReader Reader(*Stream);

  if (Stream->getLength() < sizeof(HeaderInfo))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI Stream does not contain a header.");
  if (auto EC = Reader.readObject(Header))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI Stream does not contain a header.");

  if (Header->VersionSignature != -1)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Invalid DBI version signature.");

  // Require at least version 7, which should be present in all PDBs
  // produced in the last decade and allows us to avoid having to
  // special case all kinds of complicated arcane formats.
  if (Header->VersionHeader < PdbDbiV70)
    return make_error<RawError>(raw_error_code::feature_unsupported,
                                "Unsupported DBI version.");

  auto IS = Pdb.getPDBInfoStream();
  if (!IS)
    return IS.takeError();

  if (Header->Age != IS->getAge())
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI Age does not match PDB Age.");

  if (Stream->getLength() !=
      sizeof(HeaderInfo) + Header->ModiSubstreamSize +
          Header->SecContrSubstreamSize + Header->SectionMapSize +
          Header->FileInfoSize + Header->TypeServerSize +
          Header->OptionalDbgHdrSize + Header->ECSubstreamSize)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI Length does not equal sum of substreams.");

  // Only certain substreams are guaranteed to be aligned.  Validate
  // them here.
  if (Header->ModiSubstreamSize % sizeof(uint32_t) != 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI MODI substream not aligned.");
  if (Header->SecContrSubstreamSize % sizeof(uint32_t) != 0)
    return make_error<RawError>(
        raw_error_code::corrupt_file,
        "DBI section contribution substream not aligned.");
  if (Header->SectionMapSize % sizeof(uint32_t) != 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI section map substream not aligned.");
  if (Header->FileInfoSize % sizeof(uint32_t) != 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI file info substream not aligned.");
  if (Header->TypeServerSize % sizeof(uint32_t) != 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "DBI type server substream not aligned.");

  // Since each ModInfo in the stream is a variable length, we have to iterate
  // them to know how many there actually are.
  VarStreamArray<ModInfo> ModInfoArray;
  if (auto EC = Reader.readArray(ModInfoArray, Header->ModiSubstreamSize))
    return EC;
  for (auto &Info : ModInfoArray) {
    ModuleInfos.emplace_back(Info);
  }

  if (auto EC = Reader.readStreamRef(SecContrSubstream,
                                     Header->SecContrSubstreamSize))
    return EC;
  if (auto EC = Reader.readStreamRef(SecMapSubstream, Header->SectionMapSize))
    return EC;
  if (auto EC = Reader.readStreamRef(FileInfoSubstream, Header->FileInfoSize))
    return EC;
  if (auto EC =
          Reader.readStreamRef(TypeServerMapSubstream, Header->TypeServerSize))
    return EC;
  if (auto EC = Reader.readStreamRef(ECSubstream, Header->ECSubstreamSize))
    return EC;
  if (auto EC = Reader.readArray(DbgStreams, Header->OptionalDbgHdrSize /
                                                 sizeof(ulittle16_t)))
    return EC;

  if (auto EC = initializeSectionContributionData())
    return EC;
  if (auto EC = initializeSectionHeadersData())
    return EC;
  if (auto EC = initializeSectionMapData())
    return EC;
  if (auto EC = initializeFileInfo())
    return EC;
  if (auto EC = initializeFpoRecords())
    return EC;

  if (Reader.bytesRemaining() > 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Found unexpected bytes in DBI Stream.");

  StreamReader ECReader(ECSubstream);
  if (auto EC = ECNames.load(ECReader))
    return EC;

  return Error::success();
}

PdbRaw_DbiVer DbiStream::getDbiVersion() const {
  uint32_t Value = Header->VersionHeader;
  return static_cast<PdbRaw_DbiVer>(Value);
}

uint32_t DbiStream::getAge() const { return Header->Age; }

uint16_t DbiStream::getPublicSymbolStreamIndex() const {
  return Header->PublicSymbolStreamIndex;
}

uint16_t DbiStream::getGlobalSymbolStreamIndex() const {
  return Header->GlobalSymbolStreamIndex;
}

bool DbiStream::isIncrementallyLinked() const {
  return (Header->Flags & FlagIncrementalMask) != 0;
}

bool DbiStream::hasCTypes() const {
  return (Header->Flags & FlagHasCTypesMask) != 0;
}

bool DbiStream::isStripped() const {
  return (Header->Flags & FlagStrippedMask) != 0;
}

uint16_t DbiStream::getBuildMajorVersion() const {
  return (Header->BuildNumber & BuildMajorMask) >> BuildMajorShift;
}

uint16_t DbiStream::getBuildMinorVersion() const {
  return (Header->BuildNumber & BuildMinorMask) >> BuildMinorShift;
}

uint32_t DbiStream::getPdbDllVersion() const { return Header->PdbDllVersion; }

uint32_t DbiStream::getSymRecordStreamIndex() const {
  return Header->SymRecordStreamIndex;
}

PDB_Machine DbiStream::getMachineType() const {
  uint16_t Machine = Header->MachineType;
  return static_cast<PDB_Machine>(Machine);
}

codeview::FixedStreamArray<object::coff_section>
DbiStream::getSectionHeaders() {
  return SectionHeaders;
}

codeview::FixedStreamArray<object::FpoData> DbiStream::getFpoRecords() {
  return FpoRecords;
}

ArrayRef<ModuleInfoEx> DbiStream::modules() const { return ModuleInfos; }
codeview::FixedStreamArray<SecMapEntry> DbiStream::getSectionMap() const {
  return SectionMap;
}

void llvm::pdb::DbiStream::visitSectionContributions(
    ISectionContribVisitor &Visitor) const {
  if (SectionContribVersion == DbiSecContribVer60) {
    for (auto &SC : SectionContribs)
      Visitor.visit(SC);
  } else if (SectionContribVersion == DbiSecContribV2) {
    for (auto &SC : SectionContribs2)
      Visitor.visit(SC);
  }
}

Error DbiStream::initializeSectionContributionData() {
  StreamReader SCReader(SecContrSubstream);
  if (auto EC = SCReader.readEnum(SectionContribVersion))
    return EC;

  if (SectionContribVersion == DbiSecContribVer60)
    return loadSectionContribs<SectionContrib>(SectionContribs, SCReader);
  if (SectionContribVersion == DbiSecContribV2)
    return loadSectionContribs<SectionContrib2>(SectionContribs2, SCReader);

  return make_error<RawError>(raw_error_code::feature_unsupported,
                              "Unsupported DBI Section Contribution version");
}

// Initializes this->SectionHeaders.
Error DbiStream::initializeSectionHeadersData() {
  uint32_t StreamNum = getDebugStreamIndex(DbgHeaderType::SectionHdr);
  if (StreamNum >= Pdb.getNumStreams())
    return make_error<RawError>(raw_error_code::no_stream);

  auto SHS = MappedBlockStream::createIndexedStream(StreamNum, Pdb);
  if (!SHS)
    return SHS.takeError();

  size_t StreamLen = (*SHS)->getLength();
  if (StreamLen % sizeof(object::coff_section))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Corrupted section header stream.");

  size_t NumSections = StreamLen / sizeof(object::coff_section);
  codeview::StreamReader Reader(**SHS);
  if (auto EC = Reader.readArray(SectionHeaders, NumSections))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Could not read a bitmap.");

  SectionHeaderStream = std::move(*SHS);
  return Error::success();
}

// Initializes this->Fpos.
Error DbiStream::initializeFpoRecords() {
  uint32_t StreamNum = getDebugStreamIndex(DbgHeaderType::NewFPO);

  // This means there is no FPO data.
  if (StreamNum == InvalidStreamIndex)
    return Error::success();

  if (StreamNum >= Pdb.getNumStreams())
    return make_error<RawError>(raw_error_code::no_stream);

  auto FS = MappedBlockStream::createIndexedStream(StreamNum, Pdb);
  if (!FS)
    return FS.takeError();

  size_t StreamLen = (*FS)->getLength();
  if (StreamLen % sizeof(object::FpoData))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Corrupted New FPO stream.");

  size_t NumRecords = StreamLen / sizeof(object::FpoData);
  codeview::StreamReader Reader(**FS);
  if (auto EC = Reader.readArray(FpoRecords, NumRecords))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Corrupted New FPO stream.");
  FpoStream = std::move(*FS);
  return Error::success();
}

Error DbiStream::initializeSectionMapData() {
  StreamReader SMReader(SecMapSubstream);
  const SecMapHeader *Header;
  if (auto EC = SMReader.readObject(Header))
    return EC;
  if (auto EC = SMReader.readArray(SectionMap, Header->SecCount))
    return EC;
  return Error::success();
}

Error DbiStream::initializeFileInfo() {
  struct FileInfoSubstreamHeader {
    ulittle16_t NumModules;     // Total # of modules, should match number of
                                // records in the ModuleInfo substream.
    ulittle16_t NumSourceFiles; // Total # of source files.  This value is not
                                // accurate because PDB actually supports more
                                // than 64k source files, so we ignore it and
                                // compute the value from other stream fields.
  };

  // The layout of the FileInfoSubstream is like this:
  // struct {
  //   ulittle16_t NumModules;
  //   ulittle16_t NumSourceFiles;
  //   ulittle16_t ModIndices[NumModules];
  //   ulittle16_t ModFileCounts[NumModules];
  //   ulittle32_t FileNameOffsets[NumSourceFiles];
  //   char Names[][NumSourceFiles];
  // };
  // with the caveat that `NumSourceFiles` cannot be trusted, so
  // it is computed by summing `ModFileCounts`.
  //
  const FileInfoSubstreamHeader *FH;
  StreamReader FISR(FileInfoSubstream);
  if (auto EC = FISR.readObject(FH))
    return EC;

  // The number of modules in the stream should be the same as reported by
  // the FileInfoSubstreamHeader.
  if (FH->NumModules != ModuleInfos.size())
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "FileInfo substream count doesn't match DBI.");

  FixedStreamArray<ulittle16_t> ModIndexArray;
  FixedStreamArray<ulittle16_t> ModFileCountArray;

  // First is an array of `NumModules` module indices.  This is not used for the
  // same reason that `NumSourceFiles` is not used.  It's an array of uint16's,
  // but it's possible there are more than 64k source files, which would imply
  // more than 64k modules (e.g. object files) as well.  So we ignore this
  // field.
  if (auto EC = FISR.readArray(ModIndexArray, ModuleInfos.size()))
    return EC;
  if (auto EC = FISR.readArray(ModFileCountArray, ModuleInfos.size()))
    return EC;

  // Compute the real number of source files.
  uint32_t NumSourceFiles = 0;
  for (auto Count : ModFileCountArray)
    NumSourceFiles += Count;

  // This is the array that in the reference implementation corresponds to
  // `ModInfo::FileLayout::FileNameOffs`, which is commented there as being a
  // pointer. Due to the mentioned problems of pointers causing difficulty
  // when reading from the file on 64-bit systems, we continue to ignore that
  // field in `ModInfo`, and instead build a vector of StringRefs and stores
  // them in `ModuleInfoEx`.  The value written to and read from the file is
  // not used anyway, it is only there as a way to store the offsets for the
  // purposes of later accessing the names at runtime.
  if (auto EC = FISR.readArray(FileNameOffsets, NumSourceFiles))
    return EC;

  if (auto EC = FISR.readStreamRef(NamesBuffer))
    return EC;

  // We go through each ModuleInfo, determine the number N of source files for
  // that module, and then get the next N offsets from the Offsets array, using
  // them to get the corresponding N names from the Names buffer and associating
  // each one with the corresponding module.
  uint32_t NextFileIndex = 0;
  for (size_t I = 0; I < ModuleInfos.size(); ++I) {
    uint32_t NumFiles = ModFileCountArray[I];
    ModuleInfos[I].SourceFiles.resize(NumFiles);
    for (size_t J = 0; J < NumFiles; ++J, ++NextFileIndex) {
      auto ThisName = getFileNameForIndex(NextFileIndex);
      if (!ThisName)
        return ThisName.takeError();
      ModuleInfos[I].SourceFiles[J] = *ThisName;
    }
  }

  return Error::success();
}

uint32_t DbiStream::getDebugStreamIndex(DbgHeaderType Type) const {
  return DbgStreams[static_cast<uint16_t>(Type)];
}

Expected<StringRef> DbiStream::getFileNameForIndex(uint32_t Index) const {
  StreamReader Names(NamesBuffer);
  if (Index >= FileNameOffsets.size())
    return make_error<RawError>(raw_error_code::index_out_of_bounds);

  uint32_t FileOffset = FileNameOffsets[Index];
  Names.setOffset(FileOffset);
  StringRef Name;
  if (auto EC = Names.readZeroString(Name))
    return std::move(EC);
  return Name;
}

Error DbiStream::commit() { return Error::success(); }
