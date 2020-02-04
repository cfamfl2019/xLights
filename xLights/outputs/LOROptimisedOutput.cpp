#include "LOROptimisedOutput.h"
#include "Controller.h"
#include "../OutputModelManager.h"
#include "../UtilFunctions.h"

#include <wx/xml/xml.h>
#include <wx/propgrid/propgrid.h>

#include <log4cpp/Category.hh>

wxPGChoices LOROptimisedOutput::__lorDeviceTypes;
wxPGChoices LOROptimisedOutput::__lorAddressModes;

// This is a fake dialog but it allows the delete to work
class DeleteLorControllerDialogAdapter : public wxPGEditorDialogAdapter
{
public:
    DeleteLorControllerDialogAdapter()
        : wxPGEditorDialogAdapter() {
    }
    virtual bool DoShowDialog(wxPropertyGrid* propGrid, wxPGProperty* WXUNUSED(property)) override {
        wxVariant v("");
        SetValue(v);
        return true;
    }
protected:
};

class DeleteLorControllerProperty : public wxStringProperty
{
    LorControllers& _lc;
public:
    DeleteLorControllerProperty(LorControllers& lc,
        const wxString& label,
        const wxString& name)
        : wxStringProperty(label, name, wxEmptyString), _lc(lc) {
    }
    virtual ~DeleteLorControllerProperty() { }
    const wxPGEditor* DoGetEditorClass() const
    {
        return wxPGEditor_TextCtrlAndButton;
    }
    virtual wxPGEditorDialogAdapter* GetEditorDialog() const override {
        // extract the index of the LOR device to delete
        int pos = wxAtoi(GetName().AfterLast('/'));

        // get an iterator pointing to it
        auto it = _lc.GetControllers().begin();
        std::advance(it, pos);
        wxASSERT(it != _lc.GetControllers().end());

        // now erase it
        _lc.GetControllers().erase(it);

        // return a dummy dialog which just does enough to register that a change has happened so we can trigger the refresh
        return new DeleteLorControllerDialogAdapter();
    }
};

void LOROptimisedOutput::InitialiseTypes()
{
    if (__lorAddressModes.GetCount() == 0)
    {
        __lorAddressModes.Add("Normal");
        __lorAddressModes.Add("Legacy");
        __lorAddressModes.Add("Split");
    }

    if (__lorDeviceTypes.GetCount() == 0)
    {
        __lorDeviceTypes.Add("AC Controller");
        __lorDeviceTypes.Add("RGB Controller");
        __lorDeviceTypes.Add("CCR");
        __lorDeviceTypes.Add("CCB");
        __lorDeviceTypes.Add("Pixie4");
        __lorDeviceTypes.Add("Pixie8");
        __lorDeviceTypes.Add("Pixie16");
    }
}

#pragma region Constructors and Destructors
LOROptimisedOutput::LOROptimisedOutput(SerialOutput* output)
: LOROutput(output)
{
    InitialiseTypes();
    SetupHistory();
}

LOROptimisedOutput::LOROptimisedOutput(wxXmlNode* node)
: LOROutput(node), _controllers(node)
{
    InitialiseTypes();
    SetupHistory();
    CalcTotalChannels();
}

LOROptimisedOutput::LOROptimisedOutput() : LOROutput()
{
    InitialiseTypes();
    SetupHistory();
}

LOROptimisedOutput::~LOROptimisedOutput()
{
}

void LOROptimisedOutput::SetupHistory()
{
    for (size_t i = 0; i < MAX_BANKS; ++i) {
        banks_changed[i] = true;
    }
}
#pragma endregion Constructors and Destructors

#pragma region Save
void LOROptimisedOutput::Save(wxXmlNode* node)
{
    wxXmlNode* cntrl_node = new wxXmlNode(wxXML_ELEMENT_NODE, "controllers");
    node->AddChild(cntrl_node);
    _controllers.Save(cntrl_node);
    SerialOutput::Save(node);
}
#pragma endregion Save

#pragma region Frame Handling
void LOROptimisedOutput::EndFrame(int suppressFrames)
{
    if (!_enabled || _suspend) return;

    if (_changed)
    {
        SetManyChannels(0, _curData, 0);
        _changed = false;
    }
    LOROutput::EndFrame(suppressFrames);
}
#pragma endregion Frame Handling

#pragma region Data Setting
void LOROptimisedOutput::SetOneChannel(int32_t channel, unsigned char data)
{
    if (!_enabled || _serial == nullptr || !_ok) return;

    if( !_changed ) {
        // Don't try to only send changes since this is used for test mode
        // and not all channels are written every frame
        SetupHistory();
        wxASSERT(sizeof(_curData) <= sizeof(_lastSent));
        memset(_curData, 0x00, sizeof(_curData));
        memset(_lastSent, 0xFF, sizeof(_curData));
        _changed = true;
    }

    wxASSERT(channel < sizeof(_curData));
    _curData[channel] = data;
}

void LOROptimisedOutput::SetManyChannels(int32_t channel, unsigned char data[], size_t size)
{
    if (!_enabled || _serial == nullptr || !_ok) return;

    log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    if( !TxEmpty() ) {
        logger_base.debug("    LOROptimisedOutput: SetManyChannels skipped due to transmit buffer stackup");
        return;
    }

    int cur_channel = channel;
    int total_bytes_sent = 0;

    for (const auto& it : _controllers.GetControllers())
    {
		int channel_count = it->GetNumChannels();
        int unit_id = it->GetUnitId();

        int controller_channels_to_process = channel_count;
        int channels_per_pass = controller_channels_to_process;
		CalcChannels(channel_count, channels_per_pass, controller_channels_to_process, it);

        while( controller_channels_to_process > 0 ) {
            size_t idx = 0;  // running index for placing next byte
            uint8_t d[8192];
            std::vector< std::vector<LORDataPair> > lorBankData;
            lorBankData.resize((channels_per_pass/16)+1);

            bool bank_changed = false;
            bool frame_changed = false;
            bool color_mode[MAX_BANKS];

            // gather all the data and compress common values on a per 16 channel bank basis
            int channels_to_process = channels_per_pass;
            int chan_offset = 0;
            int shift_offset = 0;
            color_mode[0] = false;
            while (channels_to_process > 0) {
                bool processed = false;

                wxASSERT(cur_channel < sizeof(cur_channel));
                if ((data[cur_channel] > 0) && (data[cur_channel] < 0xFF)) {
                    wxASSERT(shift_offset < sizeof(color_mode));
                    color_mode[shift_offset] = true;
                }

                wxASSERT(shift_offset < sizeof(lorBankData));
                for (int i = 0; i < lorBankData[shift_offset].size(); ++i) {
                    if( lorBankData[shift_offset][i].value == data[cur_channel] ) {
                        lorBankData[shift_offset][i].bits |= (1 << chan_offset);
                        processed = true;
                        break;
                    }
                }

                if (!processed) {
                    lorBankData[shift_offset].push_back(LORDataPair(data[cur_channel], 1 << chan_offset));
                }

                if (data[cur_channel] != _lastSent[cur_channel]) {
                    bank_changed = true;
                    frame_changed = true;
                }
                _lastSent[cur_channel] = data[cur_channel];

                chan_offset++;
                if (chan_offset == 16 || (channels_to_process == 1)) {
                    chan_offset = 0;
                    banks_changed[shift_offset] = bank_changed;
                    bank_changed = false;
                    shift_offset++;
                    color_mode[shift_offset] = false;
                }
                channels_to_process--;
                cur_channel++;
            }

            // now build the commands to send out the serial port
            for (int bank = lorBankData.size() - 1; bank >= 0; --bank) {
                wxASSERT(bank < sizeof(banks_changed));
                if (banks_changed[bank]) {
                    int num_bank_records = lorBankData[bank].size();

                    // process channels that are zero
                    for( int i = 0; i < num_bank_records; ++i ) {
                        if( lorBankData[bank][i].value == 0 ) {
                            if( num_bank_records == 1 ) {  // entire bank is zero
                                d[idx++] = 0;
                                d[idx++] = unit_id;
                                if( bank > 0) {
                                    d[idx++] = 0x51;
                                    d[idx++] = 0xC0 | bank;
                                }
                                else {
                                    d[idx++] = 0x41;
                                }
                            } else if( color_mode[bank] ) {
                                // send a value byte command for 0 if in color mode
                                // otherwise its either all off or all on and will be covered
                                // by other commands.
                                uint8_t lsb = lorBankData[bank][i].bits & 0xFF;
                                uint8_t msb = lorBankData[bank][i].bits >> 8;
                                d[idx++] = 0;
                                d[idx++] = unit_id;
                                d[idx++] = 0x53;
                                d[idx++] = _data[0];
                                d[idx] = bank;
                                if (!msb) {
                                    d[idx] |= 0x40;
                                }
                                else if (!lsb) {
                                    d[idx] |= 0x80;
                                }
                                idx++;
                                if (lsb) {
                                    d[idx++] = lsb;
                                }
                                if (msb) {
                                    d[idx++] = msb;
                                }
                            }
                        }
                    }

                    // send all the channels with 0xFF first
                    for (int i = 0; i < num_bank_records; ++i) {
                        if (lorBankData[bank][i].value == 0xFF) {
                            uint8_t lsb = lorBankData[bank][i].bits & 0xFF;
                            uint8_t msb = lorBankData[bank][i].bits >> 8;
                            bool value_byte = lorBankData[bank][i].value != 0xFF;
                            GenerateCommand(d, idx, unit_id, bank, value_byte, _data[lorBankData[bank][i].value], lsb, msb);
                        }
                    }

                    // now send all commands that are values between 0 and FF
                    for (int i = 0; i < num_bank_records; ++i) {
                        if ((lorBankData[bank][i].value != 0) && (lorBankData[bank][i].value != 0xFF)) {
                            uint8_t lsb = lorBankData[bank][i].bits & 0xFF;
                            uint8_t msb = lorBankData[bank][i].bits >> 8;
                            bool value_byte = lorBankData[bank][i].value != 0xFF;
                            GenerateCommand(d, idx, unit_id, bank, value_byte, _data[lorBankData[bank][i].value], lsb, msb);
                        }
                    }
                }
            }
            if( frame_changed ) {
                d[idx++] = 0x0;
                d[idx++] = 0x0;
            }

            if (_serial != nullptr && frame_changed)
            {
                _serial->Write((char *)d, idx);
                total_bytes_sent += idx;
            }

            for (int bank = 0; bank < lorBankData.size(); bank++) {
                lorBankData[bank].clear();
            }
            lorBankData.clear();
            controller_channels_to_process -= channels_per_pass;
            unit_id++;
        }
    }
    //logger_base.debug("    LOROptimisedOutput: Sent %d bytes", total_bytes_sent);
}

void LOROptimisedOutput::GenerateCommand(uint8_t d[], size_t& idx, int unit_id, int bank, bool value_byte, uint8_t dbyte, uint8_t lsb, uint8_t msb)
{
    d[idx++] = 0;        // Leading zero
    d[idx++] = unit_id;  // Unit ID
                         // command byte w/optional value & shift bytes
    if (bank > 0) {
        d[idx] = 0x51;
        if (value_byte) {
            d[idx++] |= 0x02;
            d[idx] = dbyte;
        }
        idx++;
        d[idx] = bank;
        if (!msb) {
            d[idx] |= 0x40;
        }
        else if (!lsb) {
            d[idx] |= 0x80;
        }
        idx++;
    }
    else {
        if (!msb) {
            d[idx] = 0x31;
        }
        else if (!lsb) {
            d[idx] = 0x21;
        }
        else {
            d[idx] = 0x11;
        }
        if (value_byte) {
            d[idx++] |= 0x02;
            d[idx] = dbyte;
        }
        idx++;
    }
    if (lsb) {
        d[idx++] = lsb;
    }
    if (msb) {
        d[idx++] = msb;
    }
}

void LOROptimisedOutput::AllOff()
{
    int bank = 0;
    log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("    LOROptimisedOutput: AllOff starting");

    for (const auto& it : _controllers.GetControllers())
    {
        int unit_id = it->GetUnitId();

        int channel_count = it->GetNumChannels();
        int controller_channels_to_process = channel_count;
        int channels_per_pass = controller_channels_to_process;
		CalcChannels(channel_count, channels_per_pass, controller_channels_to_process, it);

        while( controller_channels_to_process > 0 ) {
            size_t idx = 0;
            uint8_t d[1024];
            int channels_to_process = channels_per_pass;
            while (channels_to_process > 0) {
                d[idx++] = 0;
                d[idx++] = unit_id;
                if( bank > 0) {
                    d[idx++] = 0x51;
                    d[idx++] = 0xC0 | bank;
                }
                else {
                    d[idx++] = 0x41;
                }
                channels_to_process -= 16;
                bank++;
            }
            d[idx++] = 0x0;
            d[idx++] = 0x0;

            if (_serial != nullptr)
            {
                _serial->Write((char *)d, idx);
            }
            controller_channels_to_process -= channels_per_pass;
            unit_id++;
        }
    }

    SendHeartbeat();
    _lastheartbeat = _timer_msec;
    wxMilliSleep(50);
    logger_base.debug("    LOROptimisedOutput: AllOff finished");
}
#pragma endregion Data Setting

#pragma region Getters and Setters
std::string LOROptimisedOutput::GetSetupHelp() const
{
    return "LOR controllers attached to any LOR dongle.\nMax of 8 channels at 9600 baud.\nMax of 48 channels at 57600 baud.\nMax of 96 channels at 115200 baud.\nRun your controllers in DMX mode for higher throughput.";
}
#pragma endregion Getters and Setters

#pragma region UI
#ifndef EXCLUDENETWORKUI
void LOROptimisedOutput::AddProperties(wxPropertyGrid* propertyGrid, bool allSameSize)
{
    auto devs = GetControllers().GetControllers();
    auto p = propertyGrid->Append(new wxUIntProperty("Devices", "Devices", devs.size()));
    p->SetAttribute("Min", 1);
    p->SetAttribute("Max", 32);
    p->SetEditor("SpinCtrl");

    int i = 0;
    for (const auto& it : devs)
    {
        wxPGProperty* p2 = propertyGrid->Append(new wxPropertyCategory(it->GetType() + " : " + it->GetDescription(), wxString::Format("Device%d", i)));

        p = propertyGrid->AppendIn(p2, new DeleteLorControllerProperty(GetControllers(), _("Delete this device"), wxString::Format("DeleteDevice/%d", i)));
        propertyGrid->LimitPropertyEditing(p);

        p = propertyGrid->AppendIn(p2, new wxEnumProperty("Device Type", wxString::Format("DeviceType/%d", i), __lorDeviceTypes, Controller::EncodeChoices(__lorDeviceTypes, it->GetType())));

        std::string ch = "Channels";
        if (StartsWith(it->GetType(), "Pixie"))
        {
            ch = "Channels Per Port";
        }
        p = propertyGrid->AppendIn(p2, new wxUIntProperty(ch, wxString::Format("DeviceChannels/%d", i), it->GetNumChannels()));
        p->SetAttribute("Min", 1);
        p->SetAttribute("Max", it->GetMaxChannels());
        p->SetEditor("SpinCtrl");

        p = propertyGrid->AppendIn(p2, new wxUIntProperty("Unit ID", wxString::Format("DeviceUnitID/%d", i), it->GetUnitId()));
        p->SetAttribute("Min", 1);
        p->SetAttribute("Max", it->GetMaxUnitId());
        p->SetEditor("SpinCtrl");

        p = propertyGrid->AppendIn(p2, new wxStringProperty("Unit ID - Hex", wxString::Format("DeviceUnitIDHex/%d", i), wxString::Format("0x%02x", it->GetUnitId())));
        p->ChangeFlag(wxPG_PROP_READONLY, true);
        p->SetBackgroundColour(*wxLIGHT_GREY);

        if (!StartsWith(it->GetType(), "Pixie"))
        {
            p = propertyGrid->AppendIn(p2, new wxEnumProperty("Address Mode", wxString::Format("DeviceAddressMode/%d", i), __lorAddressModes, it->GetAddressMode()));
        }

        p = propertyGrid->AppendIn(p2, new wxStringProperty("Description", wxString::Format("DeviceDescription/%d", i), it->GetDescription()));

        i++;
    }
}

bool LOROptimisedOutput::HandlePropertyEvent(wxPropertyGridEvent& event, OutputModelManager* outputModelManager)
{
    wxString name = event.GetPropertyName();
    wxPropertyGrid* grid = dynamic_cast<wxPropertyGrid*>(event.GetEventObject());

    if (StartsWith(name, "DeleteDevice/"))
    {
        CalcTotalChannels();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::DeleteDevice");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_UPDATE_NETWORK_LIST, "ControllerSerial::HandlePropertyEvent::DeleteDevice");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANNELSCHANGE, "ControllerSerial::HandlePropertyEvent::DeleteDevice");
        outputModelManager->AddLayoutTabWork(OutputModelManager::WORK_CALCULATE_START_CHANNELS, "ControllerSerial::HandlePropertyEvent::DeleteDevice");
        return true;
    }
    else if (name == "Devices") {
        while (event.GetValue().GetLong() < GetControllers().GetControllers().size()) {
            delete GetControllers().GetControllers().back();
            GetControllers().GetControllers().pop_back();
        }
        while (event.GetValue().GetLong() > GetControllers().GetControllers().size()) {
            GetControllers().GetControllers().push_back(new LorController());
        }
        CalcTotalChannels();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::Devices");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_UPDATE_NETWORK_LIST, "ControllerSerial::HandlePropertyEvent::Devices");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANNELSCHANGE, "ControllerSerial::HandlePropertyEvent::Devices");
        outputModelManager->AddLayoutTabWork(OutputModelManager::WORK_CALCULATE_START_CHANNELS, "ControllerSerial::HandlePropertyEvent::Devices");
    }
    else if (StartsWith(name, "DeviceType/"))
    {
        int index = wxAtoi(wxString(name).AfterLast('/'));
        auto it = GetControllers().GetControllers().begin();
        std::advance(it, index);
        wxASSERT(it != GetControllers().GetControllers().end());
        (*it)->SetType(Controller::DecodeChoices(__lorDeviceTypes, event.GetValue().GetLong()));
        GetControllers().SetDirty();
        CalcTotalChannels();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::DeviceType");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_UPDATE_NETWORK_LIST, "ControllerSerial::HandlePropertyEvent::DeviceType");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANNELSCHANGE, "ControllerSerial::HandlePropertyEvent::DeviceType");
        outputModelManager->AddLayoutTabWork(OutputModelManager::WORK_CALCULATE_START_CHANNELS, "ControllerSerial::HandlePropertyEvent::DeviceType");
    }
    else if (StartsWith(name, "DeviceChannels/"))
    {
        int index = wxAtoi(wxString(name).AfterLast('/'));
        auto it = GetControllers().GetControllers().begin();
        std::advance(it, index);
        wxASSERT(it != GetControllers().GetControllers().end());
        (*it)->SetNumChannels(event.GetValue().GetLong());
        GetControllers().SetDirty();
        CalcTotalChannels();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::DeviceChannels");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_UPDATE_NETWORK_LIST, "ControllerSerial::HandlePropertyEvent::DeviceChannels");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANNELSCHANGE, "ControllerSerial::HandlePropertyEvent::DeviceChannels");
        outputModelManager->AddLayoutTabWork(OutputModelManager::WORK_CALCULATE_START_CHANNELS, "ControllerSerial::HandlePropertyEvent::DeviceChannels");
    }
    else if (StartsWith(name, "DeviceUnitID/")) {
        int index = wxAtoi(wxString(name).AfterLast('/'));
        auto it = GetControllers().GetControllers().begin();
        std::advance(it, index);
        wxASSERT(it != GetControllers().GetControllers().end());
        (*it)->SetUnitID(event.GetValue().GetLong());
        GetControllers().SetDirty();
        CalcTotalChannels();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::DeviceUnitID");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_UPDATE_NETWORK_LIST, "ControllerSerial::HandlePropertyEvent::DeviceUnitID");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANNELSCHANGE, "ControllerSerial::HandlePropertyEvent::DeviceUnitID");
        outputModelManager->AddLayoutTabWork(OutputModelManager::WORK_CALCULATE_START_CHANNELS, "ControllerSerial::HandlePropertyEvent::DeviceUnitID");
    }
    else if (StartsWith(name, "DeviceAddressMode/")) {
        int index = wxAtoi(wxString(name).AfterLast('/'));
        auto it = GetControllers().GetControllers().begin();
        std::advance(it, index);
        wxASSERT(it != GetControllers().GetControllers().end());
        (*it)->SetMode((LorController::AddressMode)event.GetValue().GetLong());
        GetControllers().SetDirty();
        CalcTotalChannels();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::DeviceAddressMode");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_UPDATE_NETWORK_LIST, "ControllerSerial::HandlePropertyEvent::DeviceAddressMode");
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANNELSCHANGE, "ControllerSerial::HandlePropertyEvent::DeviceAddressMode");
        outputModelManager->AddLayoutTabWork(OutputModelManager::WORK_CALCULATE_START_CHANNELS, "ControllerSerial::HandlePropertyEvent::DeviceAddressMode");
    }
    else if (StartsWith(name, "DeviceDescription/")) {
        int index = wxAtoi(wxString(name).AfterLast('/'));
        auto it = GetControllers().GetControllers().begin();
        std::advance(it, index);
        wxASSERT(it != GetControllers().GetControllers().end());
        (*it)->SetDescription(event.GetValue().GetString());
        GetControllers().SetDirty();
        outputModelManager->AddASAPWork(OutputModelManager::WORK_NETWORK_CHANGE, "ControllerSerial::HandlePropertyEvent::DeviceDescription");
    }

    if (Output::HandlePropertyEvent(event, outputModelManager)) return true;

    return false;
}
#endif
#pragma endregion UI

void LOROptimisedOutput::CalcChannels(int& channel_count, int& channels_per_pass, int& controller_channels_to_process, LorController* cntrl)
{
    LorController::AddressMode addr_mode = cntrl->GetAddressMode();
    controller_channels_to_process = channel_count;
    channels_per_pass = controller_channels_to_process;
    std::string type = cntrl->GetType();
    int unit_id = cntrl->GetUnitId();
    if ((type == "Pixie4") || (type == "Pixie8") || (type == "Pixie16")) {
        std::size_t found = type.find("Pixie");
        if (found != std::string::npos) {
            int outputs_per_card = wxAtoi(type.substr(found + 5, type.length() - found - 5));
            channels_per_pass = channel_count;
            channel_count = outputs_per_card * channels_per_pass;
            controller_channels_to_process = channel_count;
            for (int i = 0; i < outputs_per_card; i++) {
                unit_id_in_use[unit_id + i] = true;
            }
        }
    }
    else {
        if (addr_mode == LorController::LOR_ADDR_MODE_LEGACY) {
            channels_per_pass = 16;
            int num_ids = channel_count / channels_per_pass;
            if ((num_ids * channels_per_pass) < channel_count) {
                num_ids++;
            }
            for (int i = 0; i < num_ids; i++) {
                unit_id_in_use[unit_id + i] = true;
            }
        }
        else if (addr_mode == LorController::LOR_ADDR_MODE_SPLIT) {
            channels_per_pass = channel_count / 2;
            unit_id_in_use[unit_id] = true;
            unit_id_in_use[unit_id + 1] = true;
        }
    }
}

void LOROptimisedOutput::CalcTotalChannels()
{
	int total_channels = 0;
    int channel_count = 0;
	int controller_channels_to_process = 0;
	int channels_per_pass = 0;
    for( size_t i=0; i < 255; ++i ) {
        unit_id_in_use[i] = false;
    }
    unit_id_in_use[0] = true;  // we don't use id 0
    for (const auto& it : _controllers.GetControllers())
    {
        channel_count = it->GetNumChannels();
        int unit_id = it->GetUnitId();
        unit_id_in_use[unit_id] = true;
		CalcChannels(channel_count, channels_per_pass, controller_channels_to_process, it);
		total_channels += channel_count;
    }
    _channels = total_channels;
}
