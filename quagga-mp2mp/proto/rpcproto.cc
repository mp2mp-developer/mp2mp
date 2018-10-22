#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <utility>
#include <vector>
#include <string>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/compiler/parser.h>

#define MODE_BUILD  0
#define MODE_TEST   1

struct _Opt {
    int mode;
    std::string strFile;
    std::string strOutput;
} g_Opt;

std::set<std::string> g_FileProtoSet;

google::protobuf::FileDescriptorSet g_OutputSet;
google::protobuf::FileDescriptorProto* GetNewFileProto()
{
    return g_OutputSet.add_file();
}

void usage()
{
    fprintf(stderr, "usage: rpcproto [OPTIONS]\n"
                    "Options:\n"
                    "  -f, --file [files]         proto file or dir\n"
                    "  -o, --out [proto model]    output proto model file\n"
                    "      --test                 test proto message\n"
                    "      --help                 display help information\n");
}


int getopt(int argc, char* argv[])
{
    struct option longopts[] = {
        {"file", required_argument, NULL, 'f'},
        {"out", required_argument, NULL, 'o'},
        {"help", no_argument, NULL, 0x100},
        {"test", no_argument, NULL, 0x101},
        {0, 0, 0, 0}
    };

    g_Opt.mode = MODE_BUILD;

    int c;
    while((c = getopt_long(argc, argv, "f:o:", longopts, NULL)) != -1)
    {
        switch(c)
        {
        case 'f':
            g_Opt.strFile = optarg;
            break;
        case 'o':
            g_Opt.strOutput = optarg;
            break;
        case 0x101:
            g_Opt.mode = MODE_TEST;
            break;
        case 0x100:
        default:
            usage();
            return -1;
        }
    }

    if((g_Opt.mode == MODE_BUILD && (g_Opt.strOutput.empty() || g_Opt.strFile.empty())) ||
       (g_Opt.mode == MODE_TEST && g_Opt.strOutput.empty()))
    {
        usage();
        return -1;
    }

    return 0;
}

void OutputSet()
{
    std::string str;
    if(g_OutputSet.SerializeToString(&str))
    {
        FILE* file = fopen(g_Opt.strOutput.c_str(), "w");
        if(file)
        {
            fwrite(str.data(), str.length(), 1, file);
            fclose(file);
            printf("write proto to `%s`\n", g_Opt.strOutput.c_str());
        }
        else
            fprintf(stderr, "error: open output file fail, %s.\n", strerror(errno));
    }
    else
        fprintf(stderr, "error: serialize file descriptor set fail.\n");
}

bool ParseProtoFile(const char* szFile, const char* szName)
{
    char buffer[260];
    snprintf(buffer, 260, "Proto/%s", szName);
    if(g_FileProtoSet.find(buffer) != g_FileProtoSet.end())
    {
        printf("proto %s has been build\n", buffer);
        return true;
    }

    printf("process proto %s\n", buffer);

    int fd = open(szFile, O_RDONLY, 0666);
    if(fd == -1) return false;

    google::protobuf::io::FileInputStream is(fd);
    google::protobuf::io::Tokenizer tokenizer(&is, NULL);
    google::protobuf::compiler::Parser parser;

    google::protobuf::FileDescriptorProto* pFileProto = GetNewFileProto();
    if(!parser.Parse(&tokenizer, pFileProto))
    {
        close(fd);
        return false;
    }

    if(!pFileProto->has_name())
        pFileProto->set_name(buffer);

    close(fd);
    return true;
}

bool IsProtoFile(const char* szFile)
{
    int size = strlen(szFile);
    if(size <= 6) return false;
    return (0 == strcmp(&szFile[size - 6], ".proto"));
}

struct ProtoItem
{
    bool loaded;
    const google::protobuf::FileDescriptorProto* proto;
};

void DumpDescriptor(const google::protobuf::FileDescriptor* pFileDescriptor, google::protobuf::DescriptorPool& pool)
{
    int count = pFileDescriptor->message_type_count();
    for(int i=0; i<count; ++i)
    {
        const google::protobuf::Descriptor* pDescriptor = pFileDescriptor->message_type(i);
        printf("class name: %s\n", pDescriptor->name().c_str());

        google::protobuf::DynamicMessageFactory factory(&pool);
        const google::protobuf::Message* pMessage = factory.GetPrototype(pDescriptor);
        if(pMessage == NULL)
        {
            fprintf(stderr, "error: factory get prototype fail, %s\n", pDescriptor->name().c_str());
            continue;
        }

        google::protobuf::Message* pObject = pMessage->New();
        if(pObject == NULL)
        {
            fprintf(stderr, "error: create new message fail.\n");
        }
        printf("object: 0x%lx\n", (uint64_t)pObject);
        delete pObject;
    }
}

bool DepthBuildProto(const google::protobuf::FileDescriptorProto* pProto,
                     std::map<std::string, ProtoItem>& ProtoMap,
                     google::protobuf::DescriptorPool& pool)
{
    std::map<std::string, ProtoItem>::iterator iter = ProtoMap.find(pProto->name());
    if(iter->second.loaded)
        return true;

    printf("build file: %s\n", pProto->name().c_str());

    int dependency_size = pProto->dependency_size();
    for(int i=0; i<dependency_size; ++i)
    {
        const std::string& dependency = pProto->dependency(i);
        std::map<std::string, ProtoItem>::iterator depIter = ProtoMap.find(dependency);
        if(depIter == ProtoMap.end())
        {
            fprintf(stderr, "error: not found dependency %s\n", dependency.c_str());
            return false;
        }

        if(!DepthBuildProto(depIter->second.proto, ProtoMap, pool))
            return false;
    }

    const google::protobuf::FileDescriptor* descriptor = pool.BuildFile(*pProto);
    if(descriptor == NULL)
    {
        fprintf(stderr, "error: build file from FileDescriptorProto fail(name:%s)\n",
                pProto->name().c_str());
        return false;
    }

    DumpDescriptor(descriptor, pool);
    iter->second.loaded = true;
    return true;
}

int TestProtoModel()
{
    int fd = open(g_Opt.strOutput.c_str(), O_RDONLY, 0666);
    if(fd == -1)
    {
        fprintf(stderr, "error: open proto model fail, %s\n", strerror(errno));
        return -1;
    }

    google::protobuf::io::FileInputStream is(fd);
    if(!g_OutputSet.ParseFromZeroCopyStream(&is))
    {
        close(fd);
        fprintf(stderr, "error: parse file descriptor proto fail.\n");
        return -1;
    }
    close(fd);

    std::map<std::string, ProtoItem> ProtoMap;

    int size = g_OutputSet.file_size();
    for(int i=0; i<size; ++i)
    {
        const google::protobuf::FileDescriptorProto& fileDescProto = g_OutputSet.file(i);

        ProtoItem item;
        item.proto = &fileDescProto;
        item.loaded = false;
        ProtoMap.insert(std::make_pair(fileDescProto.name(), item));
    }

    google::protobuf::DescriptorPool pool;
    for(int i=0; i<size; ++i)
    {
        const google::protobuf::FileDescriptorProto& fileDescProto = g_OutputSet.file(i);
        if(!DepthBuildProto(&fileDescProto, ProtoMap, pool))
            return -1;
    }

    return 0;
}

void LoadSet()
{
    int fd = open(g_Opt.strOutput.c_str(), O_RDONLY, 0666);
    if(fd == -1) return;

    google::protobuf::io::FileInputStream is(fd);
    if(!g_OutputSet.ParseFromZeroCopyStream(&is))
    {
        close(fd);
        return;
    }
    close(fd);

    int size = g_OutputSet.file_size();
    for(int i=0; i<size; ++i)
        g_FileProtoSet.insert(g_OutputSet.file(i).name());
}

int main(int argc, char* argv[])
{
    if(getopt(argc, argv) != 0)
        return -1;

    if(g_Opt.mode == MODE_TEST)
        return TestProtoModel();

    struct stat s;
    if(-1 == stat(g_Opt.strFile.c_str(), &s))
    {
        fprintf(stderr, "error: %s\n", strerror(errno));
        return -1;
    }

    LoadSet();

    if(S_ISREG(s.st_mode) && IsProtoFile(g_Opt.strFile.c_str()))
        ParseProtoFile(g_Opt.strFile.c_str(), basename(g_Opt.strFile.c_str()));
    else if(S_ISDIR(s.st_mode))
    {
        if(strcmp(g_Opt.strFile.c_str() + (g_Opt.strFile.length() - 1), "/") != 0)
            g_Opt.strFile.append("/");

        DIR* dir = opendir(g_Opt.strFile.c_str());
        if(dir == NULL)
        {
            fprintf(stderr, "error: %s\n", strerror(errno));
            return -1;
        }

        dirent* entry = NULL;
        while((entry = readdir(dir)))
        {
            if(strcmp(entry->d_name, ".") == 0 ||
               strcmp(entry->d_name, "..") == 0)
            {
               continue;
            }

            std::string file(g_Opt.strFile);
            file.append(entry->d_name);

            if(!IsProtoFile(file.c_str()))
                continue;

            ParseProtoFile(file.c_str(), entry->d_name);
        }
        closedir(dir);
    }

    OutputSet();
    return 0;
}
