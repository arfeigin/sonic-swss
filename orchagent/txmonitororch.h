#pragma once


#include "orch.h"
#include "port.h"
#include "timer.h"
#include "portsorch.h"
#include "countercheckorch.h"
#include <iomanip>
#include <iostream>


extern "C" {
#include "sai.h"
}

#define DEFAULT_THRESHOLD 10
#define DEFAULT_POLLING_PERIOD 30

static const swss::FieldValueTuple okStatus{"Status", "OK"};
static const swss::FieldValueTuple notOkStatus{"Status", "Not OK"};

class PortTxInfo
{
    public:
        PortTxInfo(std::string oid = "", uint64_t txErrsCnt = 0, bool isOK = true) :
            m_oid(oid), m_txErrsCnt(txErrsCnt), m_isOK(isOK) {}

        void setOid(std::string oid) { m_oid = oid; }
        std::string getOid() { return m_oid; }

        void setTxErrsCnt(uint64_t txErrsCnt) { m_txErrsCnt = txErrsCnt; }
        uint64_t getTxErrsCnt() { return m_txErrsCnt; }

        void setStatuts(bool isOK) { m_isOK = isOK; }
        bool getStatus() { return m_isOK; }

    private:
        std::string m_oid;
        uint64_t m_txErrsCnt;
        bool m_isOK;
};


class TxMonitorOrch : public Orch
{
    public:
        static TxMonitorOrch& getInstance(TableConnector txMonitoringConfig,
                                          TableConnector txErrorsStatus);
        virtual void doTask(swss::SelectableTimer &timer);
        virtual void doTask(Consumer &consumer);

    private:
        TxMonitorOrch(TableConnector txMonitoringConfig,
                      TableConnector txErrorsStatus);
        virtual ~TxMonitorOrch();

        void initPortsMap();
        void initCfgTable();
        void setTimer();

        void txErrorsCheck();
        uint64_t getTxErrCnt(std::string oid, uint64_t prevTxErrCnt);
        void updateStatus(PortTxInfo& portInfo, std::string oid, bool newStatus);

        void setThreshold(uint64_t newThreshold)
        {
            SWSS_LOG_ENTER();
            if (newThreshold == m_threshold) return;
            m_threshold = newThreshold;
            SWSS_LOG_NOTICE("Threshold is set to %" PRIu64 ".", m_threshold);
        }
        uint64_t getThreshold()
        {
            SWSS_LOG_ENTER();
            return m_threshold;
        }

        void setPollingPeriod(uint32_t newPollingPeriod);
        uint32_t getPollingPeriod()
        {
            SWSS_LOG_ENTER();
            return m_pollingPeriod;
        }

        void configUpdate(const std::vector<swss::FieldValueTuple> fvs);


        uint64_t m_threshold;
        uint32_t m_pollingPeriod;
        bool m_portMapReady;
        std::unique_ptr<swss::DBConnector> m_countersDb;
        std::unique_ptr<swss::Table> m_countersTable;
        std::unique_ptr<swss::Table> m_cfgTable;
        std::unique_ptr<swss::Table> m_stateTable;
        SelectableTimer* m_timer;
        std::map<std::string, PortTxInfo> m_portsMap; // maps interface name to port info
        std::vector<swss::FieldValueTuple> m_fvs;
};
