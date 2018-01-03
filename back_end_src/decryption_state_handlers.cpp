#include "decryption_state_handlers.h"
#include "decryption_state_machine.h"
#include "x2_key_loader.h"
#include "key_file_converter.h"
#include "wad_reader_writer.h"

using namespace LibEncryptMsg;

namespace EncryptPad
{
    DecryptionContext &ToContext(LightStateMachine::StateMachineContext &ctx)
    {
        return *static_cast<DecryptionContext*>(&ctx);
    }

    bool ReadIn_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        return c.Buffer().size() == 0;
    }

    void ReadIn_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        //TODO: This buffer size if for only testing small buffers
        const size_t kBufferSize = 16;
        auto &c = ToContext(ctx);
        size_t size = std::min(kBufferSize, static_cast<size_t>(c.In().GetCount()));
        c.Buffer().resize(size);
        size = c.In().Read(c.Buffer().data(), c.Buffer().size());
        c.Buffer().resize(size);
        c.SetFilterCount(0);
    }

    bool End_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        return c.In().IsEOF() && c.Buffer().empty() && c.PendingBuffer().empty();
    }

    void End_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        ToContext(ctx).SetResult(LibEncryptMsg::PacketResult::Success);
    }

    bool ParseFormat_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        switch(c.GetFormat())
        {
            case Format::Unknown:
                break;
            case Format::GPGOrNestedWad:
                if(c.GetFilterCount() != 1)
                    return false;
                break;
            default:
                return false;
        }

        return c.Buffer().size() > 0 || c.PendingBuffer().size() > 0;
    }

    void ParseFormat_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        size_t required_bytes = c.GetFilterCount() == 1 ? 4 : 1;

        c.PendingBuffer().insert(c.PendingBuffer().end(), c.Buffer().begin(), c.Buffer().end());
        c.Buffer().clear();

        // We need more bytes
        if(c.PendingBuffer().size() < required_bytes && !c.In().IsEOF())
            return;

        if(c.GetFilterCount() == 0)
        {
            uint8_t b = c.PendingBuffer()[0];
            if(b & 0x80 && b != 0xEF)
            {
                if(c.Metadata().key_only)
                {
                    c.SetFormat(Format::GPGByKeyFile);
                }
                else
                {
                    c.SetFormat(Format::GPGOrNestedWad);
                }
            }
            else // wad starts from I or P, in which the most significant bit is not set
            {
                c.SetFormat(Format::WAD);
            }
        }
        else if(c.GetFilterCount() == 1)
        {
            std::string marker;

            if(c.PendingBuffer().size() >= 4)
            {
                marker.insert(0U, reinterpret_cast<const char*>(c.PendingBuffer().data()), 4U);
            }

            if(marker == "IWAD" || marker == "PWAD")
            {
                c.SetFormat(Format::NestedWAD);
            }
            else
            {
                c.SetFormat(Format::GPG);
            }
        }
        else
        {
            // Filter count can be only 0 or 1
            assert(false);
        }

        c.Buffer().swap(c.PendingBuffer());
    }

    bool GPG_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        if(c.GetFilterCount() > 1)
            return false;

        switch(c.GetFormat())
        {
            case Format::Empty:
            case Format::Unknown:
                return false;

            case Format::GPG:
            case Format::GPGOrNestedWad:
                if(!c.PassphraseSession())
                    return false;
                if(c.GetFilterCount() == 1)
                    return false;
                break;

            case Format::GPGByKeyFile:
                if(!c.KeyFileSession())
                    return false;
                if(c.GetFilterCount() == 1)
                    return false;
                break;

            case Format::WAD:
                if(!c.IsWADHeadFinished())
                    return false;
                if(!c.KeyFileSession())
                    return false;
                if(c.GetFilterCount() == 1)
                    return false;
                break;

            case Format::NestedWAD:
                assert(c.PassphraseSession());
                if(c.GetFilterCount() == 1 && !c.IsWADHeadFinished())
                    return false;
                if(c.GetFilterCount() == 1 && !c.KeyFileSession())
                    return false;
                break;

            default:
                break;

        }

        if(c.Buffer().size() == 0)
            return false;

        return true;
    }

    void GPG_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        using namespace LibEncryptMsg;
        auto &c = ToContext(ctx);
        MessageReader *reader = nullptr;
        switch(c.GetFormat())
        {
            case Format::GPG:
            case Format::GPGOrNestedWad:
                assert(c.GetFilterCount() == 0);
                reader = &c.PassphraseSession()->reader;
                break;

            case Format::NestedWAD:
                reader = (c.GetFilterCount() == 0)
                    ?
                    &c.PassphraseSession()->reader
                    :
                    &c.KeyFileSession()->reader;
                break;

            default:
                reader = &c.KeyFileSession()->reader;
                break;
        }

        try
        {
            if(c.In().IsEOF())
            {
                reader->Finish(c.Buffer());
            }
            else
            {
                reader->Update(c.Buffer());
            }
        }
        catch(const EmsgException &e)
        {
            c.SetResult(e.result);
            c.SetFailed(true);
        }
        c.SetFilterCount(c.GetFilterCount() + 1);
        c.SetResult(PacketResult::Success);
    }

    bool SetPwdKey_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        switch(c.GetFormat())
        {
            case Format::GPG:
            case Format::GPGOrNestedWad:
                if(!c.PassphraseSession())
                    return true;
                break;

            default:
                break;
        }
        return false;
    }

    void SetPwdKey_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        c.PassphraseSession().reset(
                new DecryptionSession(
                    c.GetEncryptParams().key_service,
                    c.GetEncryptParams().passphrase)
                );
    }
    bool WriteOut_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        if(c.Buffer().size() == 0)
            return false;

        switch(c.GetFormat())
        {
            case Format::Empty:
            case Format::Unknown:
            case Format::GPGOrNestedWad:
                return false;

            case Format::GPGByKeyFile:
            case Format::GPG:
            case Format::WAD:
                if(c.GetFilterCount() != 1)
                    return false;
                break;

            case Format::NestedWAD:
                if(c.GetFilterCount() != 2)
                    return false;
                break;

            default:
                break;
        }
        return true;
    }

    void WriteOut_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        c.Out().Write(c.Buffer().data(), c.Buffer().size());
        c.Buffer().clear();
    }

    void Fail_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        // auto &c = ToContext(ctx);
    }

    bool ReadKeyFile_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        if(c.KeyFileSession())
            return false;

        switch(c.GetFormat())
        {
            case Format::GPGByKeyFile:
                break;

            case Format::WAD:
            case Format::NestedWAD:
                if(!c.IsWADHeadFinished())
                    return false;
                break;

            default:
                return false;
        }
        return true;
    }

    void ReadKeyFile_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        PacketMetadata &metadata = c.Metadata();
        const EncryptParams &encrypt_params = c.GetEncryptParams();

        if(metadata.key_file.empty())
        {
            c.SetResult(PacketResult::KeyFileNotSpecified);
            c.SetFailed(true);
            return;
        }

        c.KeyFileSession().reset(new DecryptionSession());

        std::string empty_str;
        PacketResult result = LoadKeyFromFile(metadata.key_file,
                encrypt_params.libcurl_path ? *encrypt_params.libcurl_path : empty_str,
                encrypt_params.libcurl_parameters ? *encrypt_params.libcurl_parameters : empty_str,
                c.KeyFileSession()->own_passphrase);

        if(result != PacketResult::Success)
        {
            c.SetResult(result);
            c.SetFailed(true);
            return;
        }

        if(!DecryptKeyFileContent(c.KeyFileSession()->own_passphrase,
                    encrypt_params.key_file_encrypt_params, c.KeyFileSession()->own_passphrase))
        {
            c.SetResult(PacketResult::InvalidKeyFilePassphrase);
            c.SetFailed(true);
            return;
        }

        c.SetResult(PacketResult::Success);
    }

    bool WADHead_CanEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);

        if(c.IsWADHeadFinished())
            return false;

        if(c.Buffer().size() == 0 && c.PendingBuffer().size() == 0)
            return false;

        switch(c.GetFormat())
        {
            case Format::WAD:
            case Format::NestedWAD:
                break;

            default:
                return false;
        }
        return true;
    }

    void WADHead_OnEnter(LightStateMachine::StateMachineContext &ctx)
    {
        auto &c = ToContext(ctx);
        c.PendingBuffer().insert(c.PendingBuffer().end(), c.Buffer().begin(), c.Buffer().end());
        c.Buffer().clear();

        InPacketStreamMemory stm_in(c.PendingBuffer().data(),
                c.PendingBuffer().data() + c.PendingBuffer().size());

        uint32_t payload_offset = 0;
        uint32_t payload_size = 0;
        std::string key_file;
        PacketResult result = ParseWad(stm_in, key_file, payload_offset, payload_size);

        switch(result)
        {
            case PacketResult::Success:
                break;

            case PacketResult::InvalidOrIncompleteWadFile:
                if(c.In().IsEOF())
                {
                    c.SetResult(result);
                    c.SetFailed(true);
                }
                return;

            default:
                c.SetResult(result);
                c.SetFailed(true);
                return;
        }

        if(c.Metadata().key_file.empty())
            c.Metadata().key_file = key_file;

        c.Buffer().swap(c.PendingBuffer());
        c.Buffer().erase(c.Buffer().begin(), c.Buffer().begin() + payload_offset);
        if(payload_size != 0 && payload_size < c.Buffer().size())
        {
            //This is 3.2.1 format, in which the key string and the dictionary goes after the payload.
            //We need to trim the file
            c.Buffer().erase(c.Buffer().begin() + payload_size, c.Buffer().end());
        }
        c.SetWADHeadFinished(true);
        c.SetResult(PacketResult::Success);
    }
}

