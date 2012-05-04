/* RFA provider session.
 */

#include "session.hh"

#include <algorithm>
#include <utility>

#include "chromium/logging.hh"
#include "error.hh"
#include "rfaostream.hh"
#include "provider.hh"

using rfa::common::RFA_String;

psych::session_t::session_t (
	psych::provider_t& provider,
	const unsigned instance_id,
	const psych::session_config_t& config,
	std::shared_ptr<psych::rfa_t> rfa,
	std::shared_ptr<rfa::common::EventQueue> event_queue
	) :
	last_activity_ (boost::posix_time::microsec_clock::universal_time()),
	provider_ (provider),
	instance_id_ (instance_id),
	config_ (config),
	rfa_ (rfa),
	event_queue_ (event_queue),
	error_item_handle_ (nullptr),
	item_handle_ (nullptr),
	rwf_major_version_ (0),
	rwf_minor_version_ (0),
	is_muted_ (true),
	stream_state_ (0),
	data_state_ (0)
{
	ZeroMemory (cumulative_stats_, sizeof (cumulative_stats_));
	ZeroMemory (snap_stats_, sizeof (snap_stats_));

	std::ostringstream ss;
	ss << config_.session_name << ':';
	prefix_.assign (ss.str());
}

psych::session_t::~session_t()
{
	VLOG(3) << prefix_<< "Unregistering RFA session clients.";
	if (nullptr != item_handle_)
		omm_provider_->unregisterClient (item_handle_), item_handle_ = nullptr;
	if (nullptr != error_item_handle_)
		omm_provider_->unregisterClient (error_item_handle_), error_item_handle_ = nullptr;
	omm_provider_.reset();
	session_.reset();
}
	
bool
psych::session_t::init()
{
	last_activity_ = boost::posix_time::microsec_clock::universal_time();

/* 7.2.1 Configuring the Session Layer Package.
 */
	VLOG(3) << prefix_<< "Acquiring RFA session.";
	const RFA_String sessionName (config_.session_name.c_str(), 0, false);
	session_.reset (rfa::sessionLayer::Session::acquire (sessionName));
	if (!(bool)session_)
		return false;

/* 7.5.6 Initializing an OMM Non-Interactive Provider. */
	VLOG(3) << prefix_<< "Creating OMM provider.";
	const RFA_String publisherName (config_.publisher_name.c_str(), 0, false);
	omm_provider_.reset (session_->createOMMProvider (publisherName, nullptr));
	if (!(bool)omm_provider_)
		return false;

/* 7.5.7 Registering for Events from an OMM Non-Interactive Provider. */
/* receive error events (OMMCmdErrorEvent) related to calls to submit(). */
	VLOG(3) << prefix_<< "Registering OMM error interest.";
	rfa::sessionLayer::OMMErrorIntSpec ommErrorIntSpec;
	error_item_handle_ = omm_provider_->registerClient (event_queue_.get(), &ommErrorIntSpec, *this, nullptr /* closure */);
	if (nullptr == error_item_handle_)
		return false;

	return sendLoginRequest();
}

/* 7.3.5.3 Making a Login Request	
 * A Login request message is encoded and sent by OMM Consumer and OMM non-
 * interactive provider applications.
 */
bool
psych::session_t::sendLoginRequest()
{
	VLOG(2) << prefix_<< "Sending login request.";
	rfa::message::ReqMsg request;
	request.setMsgModelType (rfa::rdm::MMT_LOGIN);
	request.setInteractionType (rfa::message::ReqMsg::InitialImageFlag | rfa::message::ReqMsg::InterestAfterRefreshFlag);

	rfa::message::AttribInfo attribInfo;
	attribInfo.setNameType (rfa::rdm::USER_NAME);
	const RFA_String userName (config_.user_name.c_str(), 0, false);
	attribInfo.setName (userName);

/* The request attributes ApplicationID and Position are encoded as an
 * ElementList (5.3.4).
 */
	rfa::data::ElementList elementList;
	rfa::data::ElementListWriteIterator it;
	it.start (elementList);

/* DACS Application Id.
 * e.g. "256"
 */
	rfa::data::ElementEntry element;
	element.setName (rfa::rdm::ENAME_APP_ID);
	rfa::data::DataBuffer elementData;
	const RFA_String applicationId (config_.application_id.c_str(), 0, false);
	elementData.setFromString (applicationId, rfa::data::DataBuffer::StringAsciiEnum);
	element.setData (elementData);
	it.bind (element);

/* DACS Position name.
 * e.g. "localhost"
 */
	element.setName (rfa::rdm::ENAME_POSITION);
	const RFA_String position (config_.position.c_str(), 0, false);
	elementData.setFromString (position, rfa::data::DataBuffer::StringAsciiEnum);
	element.setData (elementData);
	it.bind (element);

/* Instance Id (optional).
 * e.g. "<Instance Id>"
 */
	if (!config_.instance_id.empty())
	{
		element.setName (rfa::rdm::ENAME_INST_ID);
		const RFA_String instanceId (config_.instance_id.c_str(), 0, false);
		elementData.setFromString (instanceId, rfa::data::DataBuffer::StringAsciiEnum);
		element.setData (elementData);
		it.bind (element);
	}

	it.complete();
	attribInfo.setAttrib (elementList);
	request.setAttribInfo (attribInfo);

/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
	RFA_String warningText;
	const uint8_t validation_status = request.validateMsg (&warningText);
	if (rfa::message::MsgValidationWarning == validation_status) {
		LOG(WARNING) << prefix_ << "MMT_LOGIN::validateMsg: { warningText: \"" << warningText << "\" }";
		cumulative_stats_[SESSION_PC_MMT_LOGIN_MALFORMED]++;
	} else {
		assert (rfa::message::MsgValidationOk == validation_status);
		cumulative_stats_[SESSION_PC_MMT_LOGIN_VALIDATED]++;
	}

/* Not saving the returned handle as we will destroy the provider to logout,
 * reference:
 * 7.4.10.6 Other Cleanup
 * Note: The application may call destroy() on an Event Source without having
 * closed all Event Streams. RFA will internally unregister all open Event
 * Streams in this case.
 */
	VLOG(3) << prefix_<< "Registering OMM item interest.";
	rfa::sessionLayer::OMMItemIntSpec ommItemIntSpec;
	ommItemIntSpec.setMsg (&request);
	item_handle_ = omm_provider_->registerClient (event_queue_.get(), &ommItemIntSpec, *this, nullptr /* closure */);
	cumulative_stats_[SESSION_PC_MMT_LOGIN_SENT]++;
	if (nullptr == item_handle_)
		return false;

/* Store negotiated Reuters Wire Format version information. */
	rfa::data::Map map;
	map.setAssociatedMetaInfo (*item_handle_);
	rwf_major_version_ = map.getMajorVersion();
	rwf_minor_version_ = map.getMinorVersion();
/* First session. */
	if (provider_.min_rwf_major_version_ == 0 &&
	    provider_.min_rwf_minor_version_ == 0)
	{
		LOG(INFO) << prefix_ << "RWF: { MajorVersion: " << (unsigned)rwf_major_version_ <<
					     ", MinorVersion: " << (unsigned)rwf_minor_version_ << " }";
		provider_.min_rwf_major_version_ = rwf_major_version_;
		provider_.min_rwf_minor_version_ = rwf_minor_version_;
	}
	if (((provider_.min_rwf_major_version_ == rwf_major_version_ && provider_.min_rwf_minor_version_ > rwf_minor_version_) ||
	     (provider_.min_rwf_major_version_ > rwf_major_version_)))
	{
		LOG(INFO) << prefix_ << "Degrading RWF: { MajorVersion: " << (unsigned)rwf_major_version_ <<
						       ", MinorVersion: " << (unsigned)rwf_minor_version_ << " }";
		provider_.min_rwf_major_version_ = rwf_major_version_;
		provider_.min_rwf_minor_version_ = rwf_minor_version_;
	}
	return true;
}

bool
psych::session_t::createItemStream (
	const char* name,
	rfa::sessionLayer::ItemToken** token
	)
{
	assert (nullptr != token);
	VLOG(4) << prefix_ << "Creating item stream for RIC \"" << name << "\".";
	if (!is_muted_) {
		DVLOG(4) << prefix_ << "Generating token for " << name;
		*token = &( omm_provider_->generateItemToken() );
		assert (nullptr != *token);
		cumulative_stats_[SESSION_PC_TOKENS_GENERATED]++;
		last_activity_ = boost::posix_time::microsec_clock::universal_time();
	} else {
		DVLOG(4) << prefix_ << "Not generating token for " << name << " as provider is muted.";
		assert (nullptr == *token);
	}
	return true;
}

/* 7.5.9.6 Create the OMMItemCmd object and populate it with the response
 * message.  The Cmd essentially acts as a wrapper around the response message.
 * The Cmd may be created on the heap or the stack.
 */
uint32_t
psych::session_t::send (
	rfa::common::Msg& msg,
	rfa::sessionLayer::ItemToken& token,
	void* closure
	)
{
	if (is_muted_)
		return false;

	return submit (msg, token, closure);
}

uint32_t
psych::session_t::submit (
	rfa::common::Msg& msg,
	rfa::sessionLayer::ItemToken& token,
	void* closure
	)
{
	rfa::sessionLayer::OMMItemCmd itemCmd;
	itemCmd.setMsg (msg);
/* 7.5.9.7 Set the unique item identifier. */
	itemCmd.setItemToken (&token);
/* 7.5.9.8 Write the response message directly out to the network through the
 * connection.
 */
	assert ((bool)omm_provider_);
	const uint32_t submit_status = omm_provider_->submit (&itemCmd, closure);
	cumulative_stats_[SESSION_PC_RFA_MSGS_SENT]++;
	last_activity_ = boost::posix_time::microsec_clock::universal_time();
	return submit_status;
}

void
psych::session_t::processEvent (
	const rfa::common::Event& event_
	)
{
	VLOG(1) << event_;
	cumulative_stats_[SESSION_PC_RFA_EVENTS_RECEIVED]++;
	last_activity_ = boost::posix_time::microsec_clock::universal_time();
	switch (event_.getType()) {
	case rfa::sessionLayer::OMMItemEventEnum:
		processOMMItemEvent (static_cast<const rfa::sessionLayer::OMMItemEvent&>(event_));
		break;

        case rfa::sessionLayer::OMMCmdErrorEventEnum:
                processOMMCmdErrorEvent (static_cast<const rfa::sessionLayer::OMMCmdErrorEvent&>(event_));
                break;

        default:
		cumulative_stats_[SESSION_PC_RFA_EVENTS_DISCARDED]++;
		LOG(WARNING) << prefix_ << "Uncaught: " << event_;
                break;
        }
}

/* 7.5.8.1 Handling Item Events (Login Events).
 */
void
psych::session_t::processOMMItemEvent (
	const rfa::sessionLayer::OMMItemEvent&	item_event
	)
{
	cumulative_stats_[SESSION_PC_OMM_ITEM_EVENTS_RECEIVED]++;

	const rfa::common::Msg& msg = item_event.getMsg();

/* Verify event is a response event */
	if (rfa::message::RespMsgEnum != msg.getMsgType()) {
		cumulative_stats_[SESSION_PC_OMM_ITEM_EVENTS_DISCARDED]++;
		LOG(WARNING) << prefix_ << "Uncaught: " << msg;
		return;
	}

	processRespMsg (static_cast<const rfa::message::RespMsg&>(msg));
}

void
psych::session_t::processRespMsg (
	const rfa::message::RespMsg&	reply_msg
	)
{
	cumulative_stats_[SESSION_PC_RESPONSE_MSGS_RECEIVED]++;
/* Verify event is a login response event */
	if (rfa::rdm::MMT_LOGIN != reply_msg.getMsgModelType()) {
		cumulative_stats_[SESSION_PC_RESPONSE_MSGS_DISCARDED]++;
		LOG(WARNING) << prefix_ << "Uncaught: " << reply_msg;
		return;
	}

	cumulative_stats_[SESSION_PC_MMT_LOGIN_RESPONSE_RECEIVED]++;
	const rfa::common::RespStatus& respStatus = reply_msg.getRespStatus();

/* save state */
	stream_state_ = respStatus.getStreamState();
	data_state_   = respStatus.getDataState();

	switch (stream_state_) {
	case rfa::common::RespStatus::OpenEnum:
		switch (data_state_) {
		case rfa::common::RespStatus::OkEnum:
			processLoginSuccess (reply_msg);
			break;

		case rfa::common::RespStatus::SuspectEnum:
			processLoginSuspect (reply_msg);
			break;

		default:
			cumulative_stats_[SESSION_PC_MMT_LOGIN_RESPONSE_DISCARDED]++;
			LOG(WARNING) << prefix_ << "Uncaught: " << reply_msg;
			break;
		}
		break;

	case rfa::common::RespStatus::ClosedEnum:
		processLoginClosed (reply_msg);
		break;

	default:
		cumulative_stats_[SESSION_PC_MMT_LOGIN_RESPONSE_DISCARDED]++;
		LOG(WARNING) << prefix_ << "Uncaught: " << reply_msg;
		break;
	}
}

/* 7.5.8.1.1 Login Success.
 * The stream state is OpenEnum one has received login permission from the
 * back-end infrastructure and the non-interactive provider can start to
 * publish data, including the service directory, dictionary, and other
 * response messages of different message model types.
 */
void
psych::session_t::processLoginSuccess (
	const rfa::message::RespMsg&			login_msg
	)
{
	cumulative_stats_[SESSION_PC_MMT_LOGIN_SUCCESS_RECEIVED]++;
	try {
		sendDirectoryResponse();
		resetTokens();
		LOG(INFO) << prefix_ << "Unmuting provider.";
		is_muted_ = false;

/* ignore any error */
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << prefix_ << "MMT_DIRECTORY::InvalidUsageException: { StatusText: \"" << e.getStatus().getStatusText() << "\" }";
/* cannot publish until directory is sent. */
		return;
	}
}

/* 7.5.9 Sending Response Messages Using an OMM Non-Interactive Provider.
 * 10.4.3 Providing Service Directory.
 * Immediately after a successful login, and before publishing data, a non-
 * interactive provider must publish a service directory that indicates
 * services and capabilities associated with the non-interactive provider and
 * includes information about supported domain types, the service’s state, QoS,
 * and any item group information associated with the service.
 */
bool
psych::session_t::sendDirectoryResponse()
{
	VLOG(2) << prefix_ << "Sending directory response.";

/* 7.5.9.1 Create a response message (4.2.2) */
	rfa::message::RespMsg response;

/* 7.5.9.2 Set the message model type of the response. */
	response.setMsgModelType (rfa::rdm::MMT_DIRECTORY);
/* 7.5.9.3 Set response type. */
	response.setRespType (rfa::message::RespMsg::RefreshEnum);
/* 7.5.9.4 Set the response type enumation.
 * Note type is unsolicited despite being a mandatory requirement before
 * publishing.
 */
	response.setRespTypeNum (rfa::rdm::REFRESH_UNSOLICITED);

/* 7.5.9.5 Create or re-use a request attribute object (4.2.4) */
	rfa::message::AttribInfo attribInfo;

/* DataMask: required for refresh RespMsg
 *   SERVICE_INFO_FILTER  - Static information about service.
 *   SERVICE_STATE_FILTER - Refresh or update state.
 *   SERVICE_GROUP_FILTER - Transient groups within service.
 *   SERVICE_LOAD_FILTER  - Statistics about concurrent stream support.
 *   SERVICE_DATA_FILTER  - Broadcast data.
 *   SERVICE_LINK_FILTER  - Load balance grouping.
 */
	attribInfo.setDataMask (rfa::rdm::SERVICE_INFO_FILTER | rfa::rdm::SERVICE_STATE_FILTER);
/* Name:        Not used */
/* NameType:    Not used */
/* ServiceName: Not used */
/* ServiceId:   Not used */
/* Id:          Not used */
/* Attrib:      Not used */
	response.setAttribInfo (attribInfo);

/* 5.4.4 Versioning Support.  RFA Data and Msg interfaces provide versioning
 * functionality to allow application to encode data with a connection's
 * negotiated RWF version. Versioning support applies only to OMM connection
 * types and OMM message domain models.
 */
// not std::map :(  derived from rfa::common::Data
	rfa::data::Map map;
	provider_.getServiceDirectory (map);
	response.setPayload (map);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
	RFA_String warningText;
	uint8_t validation_status = response.validateMsg (&warningText);
	if (rfa::message::MsgValidationWarning == validation_status) {
		cumulative_stats_[SESSION_PC_MMT_DIRECTORY_VALIDATED]++;
		LOG(ERROR) << prefix_ << "MMT_DIRECTORY::validateMsg: { warningText: \"" << warningText << "\" }";
	} else {
		cumulative_stats_[SESSION_PC_MMT_DIRECTORY_MALFORMED]++;
		assert (rfa::message::MsgValidationOk == validation_status);
	}

/* Create and throw away first token for MMT_DIRECTORY. */
	submit (static_cast<rfa::common::Msg&> (response), omm_provider_->generateItemToken(), nullptr);
	cumulative_stats_[SESSION_PC_MMT_DIRECTORY_SENT]++;
	return true;
}

/* Iterate through entire item dictionary and re-generate tokens.
 */
bool
psych::session_t::resetTokens()
{
	if (!(bool)omm_provider_) {
		LOG(WARNING) << prefix_ << "Reset tokens whilst invalid provider.";
		return false;
	}

	LOG(INFO) << prefix_ << "Resetting " << provider_.directory_.size() << " provider tokens";
/* Cannot use std::for_each (auto λ) due to language limitations. */
	std::for_each (provider_.directory_.begin(), provider_.directory_.end(),
		[&](std::pair<std::string, std::weak_ptr<item_stream_t>> it)
	{
		if (auto sp = it.second.lock()) {
			sp->token[instance_id_] = &( omm_provider_->generateItemToken() );
			assert (nullptr != sp->token[instance_id_]);
			cumulative_stats_[SESSION_PC_TOKENS_GENERATED]++;
		}
	});
	return true;
}

/* 7.5.8.1.2 Other Login States.
 * All connections are down. The application should stop publishing; it may
 * resume once the data state becomes OkEnum.
 */
void
psych::session_t::processLoginSuspect (
	const rfa::message::RespMsg&	suspect_msg
	)
{
	cumulative_stats_[SESSION_PC_MMT_LOGIN_SUSPECT_RECEIVED]++;
	is_muted_ = true;
}

/* 7.5.8.1.2 Other Login States.
 * The login failed, and the provider application failed to get permission
 * from the back-end infrastructure. In this case, the provider application
 * cannot start to publish data.
 */
void
psych::session_t::processLoginClosed (
	const rfa::message::RespMsg&	logout_msg
	)
{
	cumulative_stats_[SESSION_PC_MMT_LOGIN_CLOSED_RECEIVED]++;
	LOG(INFO) << prefix_ << "Muting provider.";
	is_muted_ = true;
}

/* 7.5.8.2 Handling CmdError Events.
 * Represents an error Event that is generated during the submit() call on the
 * OMM non-interactive provider. This Event gives the provider application
 * access to the Cmd, CmdID, closure and OMMErrorStatus for the Cmd that
 * failed.
 */
void
psych::session_t::processOMMCmdErrorEvent (
	const rfa::sessionLayer::OMMCmdErrorEvent& error
	)
{
	cumulative_stats_[SESSION_PC_OMM_CMD_ERRORS]++;
	LOG(ERROR) << prefix_ << "OMMCmdErrorEvent: { "
		"CmdId: " << error.getCmdID() <<
		", State: " << error.getStatus().getState() <<
		", StatusCode: " << error.getStatus().getStatusCode() <<
		", StatusText: \"" << error.getStatus().getStatusText() << "\" }";
}

/* eof */
