#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "BitcoinD.h"
#include "Mixins.h"
#include "Options.h"
#include "Storage.h"
#include "SrvMgr.h"

#include <atomic>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

class CtlTask;

class Controller : public Mgr, public ThreadObjectMixin, public TimersByNameMixin, public ProcessAgainMixin
{
    Q_OBJECT
public:
    explicit Controller(const std::shared_ptr<Options> & options);
    ~Controller() override;

    void startup() override; ///< may throw
    void cleanup() override;

    int polltime_ms = 5 * 1000; ///< the default amount of time for polling bitcoind for new headers

signals:
    /// Emitted whenever bitcoind is detected to be up-to-date, and everything is synched up.
    /// note this is not emitted during regular polling, but only after `synchronizing` was emitted previously.
    void upToDate();
    /// Emitted whenever we begin synching to bitcoind. After this completes successfully, upToDate will be emitted
    /// exactly once.
    /// This signal may be emitted multiple times if there were errors and we are periodically retrying.
    void synchronizing();
    /// Emitted whenever we failed to synchronize to bitcoind.
    void synchFailure();

protected:
    Stats stats() const override;

protected slots:
    void process(bool beSilentIfUpToDate); ///< generic callback to advance state
    void process() override { process(false); } ///< from ProcessAgainMixin

private:
    friend class CtlTask;
    /// \brief newTask - Create a specific task using this template factory function. The task will be auto-started the
    ///        next time this thread enters the event loop, via a QTimer::singleShot(0,...).
    ///
    /// \param connectErroredSignal If true, auto-connect signal CtlTask::errored() to this->genericTaskErrored()
    /// \param args The rest of the args get passed to the c'tor of the concrete class specified (in the template arg).
    /// \return Returns the newly constructed CtrlTask* subclass. Note the task will start as soon as control returns
    ///         to this thread's event loop, and the task is already emplaced into the `tasks` map when this function
    ///         returns.
    template <typename CtlTaskT, typename ...Args,
              typename = std::enable_if_t<std::is_base_of_v<CtlTask, CtlTaskT>> >
    CtlTaskT *newTask(bool connectErroredSignal, Args && ...args);
    /// remove and stop a task (called after task finished() signal fires)
    void rmTask(CtlTask *);
    /// returns true iff t is not in the tasks list
    bool isTaskDeleted(CtlTask *t) const;

    /// The default 'errored' handler used if a task was created with connectErroredSignal=true in newTask above.
    void genericTaskErrored();
    static constexpr auto pollTimerName = "pollForNewHeaders";

    const std::shared_ptr<Options> options;
    std::unique_ptr<Storage> storage;
    std::unique_ptr<SrvMgr> srvmgr; ///< NB: this may be nullptr if we haven't yet synched up and started listening.
    std::unique_ptr<BitcoinDMgr> bitcoindmgr;

    struct StateMachine;
    std::unique_ptr<StateMachine> sm;

    std::unordered_map<CtlTask *, std::unique_ptr<CtlTask>> tasks;

    void add_DLHeaderTask(unsigned from, unsigned to, size_t nTasks);

    size_t nHeadersDownloadedSoFar() const; ///< not 100% accurate. call this only from this thread
};

/// Abstract base class for our private internal tasks. Concrete implementations are in Controller.cpp.
class CtlTask : public QObject, public ThreadObjectMixin, public ProcessAgainMixin
{
    Q_OBJECT
public:
    CtlTask(Controller *ctl, const QString &name = "UnnamedTask");
    ~CtlTask() override;

    int errorCode = 0;
    QString errorMessage = "";

    std::atomic<double> lastProgress = 0.0;

    const qint64 ts = Util::getTime(); ///< timestamp -- when the task was created

    using ThreadObjectMixin::start;
    using ThreadObjectMixin::stop;

signals:
    void started();
    void finished();
    void success();
    void errored();
    void progress(double); ///< some tasks emit this to indicate progress. may be a number from 0->1.0 or anything else (task specific)
protected:
    void on_started() override;
    void on_finished() override;

    void process() override = 0; ///< from ProcessAgainMixin -- here to illustrate it's still pure virtual

    virtual void on_error(const RPC::Message &);
    virtual void on_failure(const RPC::Message::Id &, const QString &msg);

    quint64 submitRequest(const QString &method, const QVariantList &params, const BitcoinDMgr::ResultsF &resultsFunc);

    Controller * const ctl;  ///< initted in c'tor
};

#endif // CONTROLLER_H