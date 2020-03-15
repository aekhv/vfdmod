#include <getopt.h>
#include <signal.h>
#include <modbus/modbus-rtu.h>
#include <QVector>
#include <QFileInfo>
#include "structures.h"

const char *github="Sources: https://github.com/aekhv/vfdmod/\n";
const char *copyright = "2020 (c) Alexander E. (Khabarovsk, Russia)\n";

int checkFlag, debugFlag, exitFlag, newFlag;
QString exeName;
hal_main_data_t *hal_mdata;
hal_user_data_t **hal_udata;

const char *short_options = "cdhn";
const struct option long_options[] = {
    {"check",   no_argument, 0, 'c'},
    {"debug",   no_argument, 0, 'd'},
    {"help",    no_argument, 0, 'h'},
    {"new",     no_argument, 0, 'n'},
    {"version", no_argument, 0, 'v'},
    {0, 0, 0, 0}
};

int load_config(const QString &inifile, main_config_t &mconfig, QVector<user_config_t> &uconfig);
int write_blank_config(const QString &inifile);

void print_help()
{
    printf("Usage:\n"
           "\t%s [keys] CONFIGFILE\n",
           qPrintable(exeName));
    printf("Keys:\n"
           "\t-c, --check\tCheck config file for errors.\n"
           "\t-d, --debug\tEnable debug mode.\n"
           "\t-h, --help\tPrint this help.\n"
           "\t-n, --new\tWrite blank config file.\n"
           "\t--version\tPrint program's version.\n");
    printf("Description:\n"
           "\tBlah-blah-blah and blah-blah-blah...\n");
    printf("Examples:\n"
           "\t%s config.ini\n"
           "\t%s --new config.ini\n"
           "\t%s --check config.ini\n",
           qPrintable(exeName),
           qPrintable(exeName),
           qPrintable(exeName));
}

void protocol_delay(const rs485_config_t &cfg)
{
    int startBit = 1;
    if (cfg.parity != "N")
        startBit++;
    long ns = 1000000000l * cfg.protocolDelay * (startBit + cfg.dataBits + cfg.stopBits) / cfg.baudRate;
    struct timespec loop_timespec = {0, ns};
    nanosleep(&loop_timespec, NULL);
}

void loop_delay(const rs485_config_t &cfg)
{
    long ns = 1000000l * cfg.loopDelayMs;
    struct timespec loop_timespec = {0, ns};
    nanosleep(&loop_timespec, NULL);
}

void closeRequest(int param) {
    Q_UNUSED(param)
    exitFlag = 1;
    printf("%s: close request received.\n", qPrintable(exeName));
}

int read_parameters(modbus_t *ctx, main_config_t &mconfig, QVector<user_config_t> &uconfig)
{
    int result;
    uint16_t value;

    /* Reading spindle output speed */
    protocol_delay(mconfig.rs485);
    result = modbus_read_registers(ctx, mconfig.rpmOut.address, 1, &value);
    if (result < 0)
        goto fail;
    *hal_mdata->spindleRpmOut = value * mconfig.rpmOut.multiplier / mconfig.rpmOut.divider;

    /* Reading user parameters */
    for (int i = 0; i < uconfig.count(); ++i) {
        protocol_delay(mconfig.rs485);
        result = modbus_read_registers(ctx, uconfig.at(i).address, 1, &value);
        if (result < 0)
            goto fail;
        switch (uconfig.at(i).pinType) {
        case HAL_FLOAT:
            *hal_udata[i]->floatPin = value * uconfig.at(i).multiplier / uconfig.at(i).divider;
            break;
        case HAL_S32:
            *hal_udata[i]->s32Pin = value * uconfig.at(i).multiplier / uconfig.at(i).divider;
            break;
        case HAL_U32:
            *hal_udata[i]->u32Pin = value * uconfig.at(i).multiplier / uconfig.at(i).divider;
            break;
        default:
            printf("%s: incorrect HAL pin type!\n", qPrintable(exeName));
            goto fail;
        }
        if (result != 0)
            goto fail;
    }

    return result;
fail:
    printf("(%d) %s\n", errno, modbus_strerror(errno));
    (*hal_mdata->errorCount)++;
    *hal_mdata->lastError = errno;
    return result;
}

int main(int argc, char *argv[])
{
    main_config_t mconfig;
    QVector<user_config_t> uconfig;
    QString inifile;

    checkFlag = 0;
    debugFlag = 0;
    exitFlag = 0;
    newFlag = 0;
    exeName = QFileInfo(argv[0]).fileName();

    int arg;
    int index;
    while ((arg = getopt_long(argc, argv, short_options, long_options, &index)) != -1) {
        switch (arg) {
        case 'c':
            checkFlag = 1;
            break;
        case 'd':
            debugFlag = 1;
            break;
        case 'h':
            print_help();
            return 0;
        case 'n':
            newFlag = 1;
            break;
        case 'v':
            printf("%s %s\n", APP_TARGET, APP_VERSION);
            printf(github);
            printf(copyright);
            return 0;
        default:
            printf("Arguments are wrong! Type '%s -h' for help.\n", qPrintable(exeName));
            return 0;
        }
    }

    if ((argc - optind) == 0) {
        printf("Critical argument is missing! Type '%s -h' for help.\n", qPrintable(exeName));
        return 0;
    }

    if ((argc - optind) > 1) {
        printf("Too many arguments! Type '%s -h' for help.\n", qPrintable(exeName));
        return 0;
    }

    inifile = argv[optind];

    /* If --check flag specified */
    if (checkFlag) {
        return load_config(inifile, mconfig, uconfig);
    }

    /* If --new flag specified */
    if (newFlag) {
        return write_blank_config(inifile);
    }

    /* Finally, trying to load an existing config */
    int result = load_config(inifile, mconfig, uconfig);
    if (result < 0)
        return result;

    /* HAL init */
    int hal_comp_id = hal_init(qPrintable(mconfig.componentName));
    if (hal_comp_id < 0)
        return -1;

    /* HAL memory allocation for main parameters */
    hal_mdata = (hal_main_data_t *)hal_malloc(sizeof(hal_main_data_t));
    if (!hal_mdata)
        goto fail;

    if (0 != hal_pin_bit_newf(HAL_OUT, &(hal_mdata->isConnected), hal_comp_id,
                          "%s.rs485.is-connected", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_s32_newf(HAL_OUT, &(hal_mdata->errorCount), hal_comp_id,
                          "%s.rs485.error-count", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_s32_newf(HAL_OUT, &(hal_mdata->lastError), hal_comp_id,
                              "%s.rs485.last-error", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_float_newf(HAL_IN, &(hal_mdata->spindleRpmIn), hal_comp_id,
                                "%s.spindle.speed-rpm-in", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_float_newf(HAL_OUT, &(hal_mdata->spindleRpmOut), hal_comp_id,
                                "%s.spindle.speed-rpm-out", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_bit_newf(HAL_OUT, &(hal_mdata->atSpeed), hal_comp_id,
                              "%s.spindle.at-speed", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_bit_newf(HAL_IN, &(hal_mdata->runForward), hal_comp_id,
                              "%s.spindle.run-forward", qPrintable(mconfig.componentName)))
        goto fail;
    if (0 != hal_pin_bit_newf(HAL_IN, &(hal_mdata->runReverse), hal_comp_id,
                              "%s.spindle.run-reverse", qPrintable(mconfig.componentName)))
        goto fail;

    *hal_mdata->isConnected = 0;
    *hal_mdata->errorCount = 0;
    *hal_mdata->lastError = 0;
    *hal_mdata->spindleRpmIn = 0;
    *hal_mdata->spindleRpmOut = 0;
    *hal_mdata->atSpeed = 0;
    *hal_mdata->runForward = 0;
    *hal_mdata->runReverse = 0;

    /* HAL memory allocation for user parameters */
    hal_udata = new hal_user_data_t*[uconfig.count()];
    for (int i = 0; i < uconfig.count(); ++i) {
        hal_udata[i] = (hal_user_data_t *)hal_malloc(sizeof(hal_user_data_t));
        if (!hal_udata[i])
            goto fail;
    }

    for (int i = 0; i < uconfig.count(); ++i) {
        switch (uconfig.at(i).pinType) {
        case HAL_FLOAT:
            if (0 == hal_pin_float_newf(HAL_OUT, &(hal_udata[i]->floatPin),
                                        hal_comp_id,
                                        "%s.parameters.%s",
                                        qPrintable(mconfig.componentName),
                                        qPrintable(uconfig.at(i).pinName)))
                *hal_udata[i]->floatPin = 0;
            else
                goto fail;
            break;
        case HAL_S32:
            if (0 == hal_pin_s32_newf(HAL_OUT, &(hal_udata[i]->s32Pin),
                                      hal_comp_id,
                                      "%s.parameters.%s",
                                      qPrintable(mconfig.componentName),
                                      qPrintable(uconfig.at(i).pinName)))
                *hal_udata[i]->s32Pin = 0;
            else
                goto fail;
            break;
        case HAL_U32:
            if (0 == hal_pin_u32_newf(HAL_OUT, &(hal_udata[i]->u32Pin),
                                      hal_comp_id,
                                      "%s.parameters.%s",
                                      qPrintable(mconfig.componentName),
                                      qPrintable(uconfig.at(i).pinName)))
                *hal_udata[i]->u32Pin = 0;
            else
                goto fail;
            break;
        default:
            printf("%s: incorrect HAL pin type!\n", qPrintable(exeName));
            goto fail;
        }
        if (result != 0)
            goto fail;
    }

    hal_ready(hal_comp_id);

    modbus_t *ctx;
    ctx = modbus_new_rtu(qPrintable(mconfig.rs485.serialDevice),
                         mconfig.rs485.baudRate,
                         mconfig.rs485.parity.at(0).toAscii(),
                         mconfig.rs485.dataBits,
                         mconfig.rs485.stopBits);
    modbus_set_debug(ctx, debugFlag);
    modbus_set_slave(ctx, mconfig.rs485.slaveAddress);
    modbus_connect(ctx);

    signal(SIGINT, closeRequest);
    signal(SIGKILL, closeRequest);
    signal(SIGTERM, closeRequest);

    while (!exitFlag) {
        read_parameters(ctx, mconfig, uconfig);
        loop_delay(mconfig.rs485);
    }

    modbus_close(ctx);
    modbus_free(ctx);

    hal_exit(hal_comp_id);
    delete [] hal_udata;
    printf("%s: application closed.\n", qPrintable(exeName));
    return 0;
fail:
    hal_exit(hal_comp_id);
    delete [] hal_udata;
    printf("%s: critical error.\n", qPrintable(exeName));
    return -1;
}
