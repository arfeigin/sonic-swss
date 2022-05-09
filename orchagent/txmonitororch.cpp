#include "txmonitororch.h"
#include "sai_serialize.h"

extern sai_port_api_t *sai_port_api;

extern PortsOrch *gPortsOrch;

using namespace std;
using namespace swss;

TxMonitorOrch& TxMonitorOrch::getInstance(TableConnector txMonitoringConfig,
                                          TableConnector txErrorsStatus)
{
    SWSS_LOG_ENTER();

    static TxMonitorOrch *instance = new TxMonitorOrch
            (txMonitoringConfig, txErrorsStatus);
    return *instance;
}

TxMonitorOrch::TxMonitorOrch(TableConnector txMonitoringConfig,
                             TableConnector txErrorsStatus) :
        Orch(txMonitoringConfig.first, txMonitoringConfig.second),
        m_threshold(DEFAULT_THRESHOLD),
        m_pollingPeriod(DEFAULT_POLLING_PERIOD),
        m_portMapReady(false),
        m_countersDb(make_unique<DBConnector>("COUNTERS_DB", 0)),
        m_countersTable(make_unique<Table>(m_countersDb.get(), COUNTERS_TABLE)),
        m_cfgTable(make_unique<Table>(txMonitoringConfig.first, txMonitoringConfig.second)),
        m_stateTable(make_unique<Table>(txErrorsStatus.first, txErrorsStatus.second))
{
    SWSS_LOG_ENTER();

    setTimer();
    initCfgTable();
    // ports might not be ready so it will be initialised when needed and ready

    SWSS_LOG_NOTICE("TxMonitorOrch initalised.");
}

TxMonitorOrch::~TxMonitorOrch(void)
{
    SWSS_LOG_ENTER();

    m_portsMap.clear();
}

void TxMonitorOrch::initCfgTable()
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fvs;
    fvs.emplace_back("Threshold", to_string(m_threshold));
    fvs.emplace_back("Polling period", to_string(m_pollingPeriod));
    m_cfgTable->set("Config", fvs);

    SWSS_LOG_NOTICE("Configuration initalised with default threshold and polling period");
    SWSS_LOG_NOTICE("Threshold is set to %" PRIu64 " and default polling period is set to %ud seconds.",
                    m_threshold, m_pollingPeriod);
}

void TxMonitorOrch::initPortsMap()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("Initalising port map and status table");

    if (!gPortsOrch->allPortsReady())
    {
        SWSS_LOG_NOTICE("Ports not ready yet.");
        return;
    }

    m_fvs.push_back(okStatus);

    map<string, Port>& ports = gPortsOrch->getAllPorts();
    for (auto const &currPort : ports)
    {
        if (currPort.second.m_type != Port::Type::PHY)
        {
            continue;
        }
        uint64_t portId = currPort.second.m_port_id;
        string oid = sai_serialize_object_id(portId);
        string ifaceName = currPort.first;

        m_portsMap.emplace(ifaceName, PortTxInfo(oid));

        // init status table
        m_stateTable->set(ifaceName, m_fvs);
    }
    m_fvs.pop_back();

    m_portMapReady = true;
    SWSS_LOG_NOTICE("Ports map ready");
}

void TxMonitorOrch::setTimer()
{
    SWSS_LOG_ENTER();

    auto interval = timespec { .tv_sec = m_pollingPeriod, .tv_nsec = 0 };
    if (m_timer == nullptr)
    {
        m_timer = new SelectableTimer(interval);
        auto executor = new ExecutableTimer(m_timer, this, "TX_ERRORS_COUNTERS_POLL");
        Orch::addExecutor(executor);
        m_timer->start();
    }
    else
    {
        m_timer->setInterval(interval);
        m_timer->reset();
    }
}

void TxMonitorOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();
    if (m_portsMap.empty())
    {
        initPortsMap(); // also initiates state table
    }
    if (!m_portMapReady) return;
    txErrorsCheck();
}

void TxMonitorOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple updates = it->second;
        const std::string & key = kfvKey(updates);
        const std::string & op = kfvOp(updates);

        if (key == "Config" && op == SET_COMMAND)
        {
            configUpdate(kfvFieldsValues(updates));
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation!");
        }
        it = consumer.m_toSync.erase(it);
    }
}

void TxMonitorOrch::configUpdate(const vector<FieldValueTuple> fvs)
{
    SWSS_LOG_ENTER();

    for(FieldValueTuple fv : fvs)
    {
        string field = fvField(fv);
        string value = fvValue(fv);

        if(field == "threshold")
        {
            setThreshold(stoull(value));
        }
        else if(field == "polling_period")
        {
            setPollingPeriod(static_cast<uint32_t>(stoul(value)));
        }
        else
        {
            SWSS_LOG_ERROR("Unknown field!");
            SWSS_LOG_ERROR("field = %s", field.c_str());
        }
    }
}

void TxMonitorOrch::setPollingPeriod(uint32_t newPollingPeriod)
{
    SWSS_LOG_ENTER();

    if (m_pollingPeriod == newPollingPeriod) return;

    m_pollingPeriod = newPollingPeriod;
    setTimer();

    SWSS_LOG_NOTICE("Polling period is now set to %ud seconds.", m_pollingPeriod);
}

void TxMonitorOrch::txErrorsCheck()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("Polling TX error counters and updating status.");

    for (auto &currPort : m_portsMap)
    {
        string ifaceName = currPort.first;
        PortTxInfo& currPortInfo = currPort.second;
        string oid = currPortInfo.getOid();
        uint64_t prevTxErrCnt = currPortInfo.getTxErrsCnt();
        uint64_t newTxErrCnt = getTxErrCnt(oid, prevTxErrCnt);

        bool newStatus = (newTxErrCnt - prevTxErrCnt <= m_threshold);

        updateStatus(currPortInfo, ifaceName, newStatus);
        currPortInfo.setTxErrsCnt(newTxErrCnt);
    }
}

inline uint64_t TxMonitorOrch::getTxErrCnt(string oid, uint64_t prevTxErrCnt)
{
    SWSS_LOG_ENTER();

    string newTxErrCntStr;
    if (!m_countersTable->hget(oid, "SAI_PORT_STAT_IF_OUT_ERRORS", newTxErrCntStr))
    {
        return prevTxErrCnt;
    }
    uint64_t newTxErrCnt = stoull(newTxErrCntStr);
    return newTxErrCnt;
}

inline void TxMonitorOrch::updateStatus(PortTxInfo &portInfo, string ifaceName, bool newStatus)
{
    SWSS_LOG_ENTER();

    bool prevStatus = portInfo.getStatus();

    // Status will be updated only if it has changed.
    if (newStatus != prevStatus)
    {
        SWSS_LOG_NOTICE("status updated for interface %s", ifaceName.c_str());
        portInfo.setStatuts(newStatus);
        if (newStatus) // meaning that the status is OK
        {
            m_fvs.push_back(okStatus);
        }
        else
        {
            m_fvs.push_back(notOkStatus);
        }
        m_stateTable->set(ifaceName, m_fvs);
        m_fvs.pop_back();
    }
    else
    {
        SWSS_LOG_NOTICE("status not updated for interface %s", ifaceName.c_str());
    }
}
