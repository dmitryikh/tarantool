-- log.lua
--
-- vim: ts=4 sw=4 et

local ffi = require('ffi')
ffi.cdef[[
    typedef void (*sayfunc_t)(int level, const char *filename, int line,
               const char *error, const char *format, ...);

    enum say_logger_type {
        SAY_LOGGER_BOOT,
        SAY_LOGGER_STDERR,
        SAY_LOGGER_FILE,
        SAY_LOGGER_PIPE,
        SAY_LOGGER_SYSLOG
    };

    enum say_logger_type
    log_type();

    void
    say_set_log_level(int new_level);

    void
    say_set_log_format(enum say_format format);

    extern void
    say_logger_init(const char *init_str, int level, int nonblock,
                    const char *format, int background);

    extern bool
    say_logger_initialized(void);

    extern sayfunc_t _say;
    extern struct ev_loop;
    extern struct ev_signal;

    extern void
    say_logrotate(struct ev_loop *, struct ev_signal *, int);

    enum say_level {
        S_FATAL,
        S_SYSERROR,
        S_ERROR,
        S_CRIT,
        S_WARN,
        S_INFO,
        S_VERBOSE,
        S_DEBUG
    };

    enum say_format {
        SF_PLAIN,
        SF_JSON
    };
    pid_t log_pid;
    extern int log_level;
    extern int log_format;
]]

local S_WARN = ffi.C.S_WARN
local S_INFO = ffi.C.S_INFO
local S_VERBOSE = ffi.C.S_VERBOSE
local S_DEBUG = ffi.C.S_DEBUG
local S_ERROR = ffi.C.S_ERROR

local json = require("json").new()
json.cfg{
    encode_invalid_numbers = true,
    encode_load_metatables = true,
    encode_use_tostring    = true,
    encode_invalid_as_nil  = true,
}

local special_fields = {
    "file",
    "level",
    "pid",
    "line",
    "cord_name",
    "fiber_name",
    "fiber_id",
    "error_msg"
}

--
-- Map format number to string.
local fmt_num2str = {
    [ffi.C.SF_PLAIN]    = "plain",
    [ffi.C.SF_JSON]     = "json",
}

--
-- Map format string to number.
local fmt_str2num = {
    ["plain"]           = ffi.C.SF_PLAIN,
    ["json"]            = ffi.C.SF_JSON,
}

local function fmt_list()
    local keyset = {}
    for k,_ in pairs(fmt_str2num) do
        keyset[#keyset+1] = k
    end
    return table.concat(keyset,',')
end

--
-- Default options. The keys are part of
-- user API, so change with caution.
local default_cfg = {
    log             = nil,
    log_nonblock    = nil,
    log_level       = S_INFO,
    log_format      = fmt_num2str[ffi.C.SF_PLAIN],
}

local cfg = setmetatable(default_cfg, {
    __newindex = function()
        error('log: Attempt to modify a read-only table')
    end,
})

local function say(level, fmt, ...)
    if ffi.C.log_level < level then
        -- don't waste cycles on debug.getinfo()
        return
    end
    local type_fmt = type(fmt)
    local format = "%s"
    if select('#', ...) ~= 0 then
        local stat
        stat, fmt = pcall(string.format, fmt, ...)
        if not stat then
            error(fmt, 3)
        end
    elseif type_fmt == 'table' then
        -- ignore internal keys
        for _, field in ipairs(special_fields) do
            fmt[field] = nil
        end
        fmt = json.encode(fmt)
        if ffi.C.log_format == ffi.C.SF_JSON then
            -- indicate that message is already encoded in JSON
            format = fmt_num2str[ffi.C.SF_JSON]
        end
    elseif type_fmt ~= 'string' then
        fmt = tostring(fmt)
    end

    local debug = require('debug')
    local frame = debug.getinfo(3, "Sl")
    local line, file = 0, 'eval'
    if type(frame) == 'table' then
        line = frame.currentline or 0
        file = frame.short_src or frame.src or 'eval'
    end

    ffi.C._say(level, file, line, nil, format, fmt)
end

local function say_closure(lvl)
    return function (fmt, ...)
        say(lvl, fmt, ...)
    end
end

local function log_rotate()
    ffi.C.say_logrotate(nil, nil, 0)
end

local function log_level(level)
    ffi.C.say_set_log_level(level)
    rawset(cfg, 'log_level', level)
end

local function log_format(name)
    if not fmt_str2num[name] then
        local m = "log_format: expected %s"
        error(m:format(fmt_list()))
    end

    if fmt_str2num[name] == ffi.C.SF_JSON then
        if ffi.C.log_type() == ffi.C.SAY_LOGGER_SYSLOG or
            ffi.C.log_type() == ffi.C.SAY_LOGGER_BOOT then
            local m = "log_format: %s can't be used with " ..
                    "syslog or boot-time logger"
            error(m:format(fmt_num2str[ffi.C.SF_JSON]))
        end
        ffi.C.say_set_log_format(ffi.C.SF_JSON)
    else
        ffi.C.say_set_log_format(ffi.C.SF_PLAIN)
    end
    rawset(cfg, 'log_format', name)
end

local function log_pid()
    return tonumber(ffi.C.log_pid)
end

--
-- Initialize logger early (if not yet set up
-- via box.cfg interface.
--
local function init(args)
    if ffi.C.say_logger_initialized() == true then
        error("log: the logger is already initialized")
    end

    args = args or {}

    if args.log_format ~= nil then
        if fmt_str2num[args.log_format] == nil then
            local m = "log: 'log_format' must be %s"
            error(m:format(fmt_list()))
        end
    else
        args.log_format = cfg.log_format
    end

    args.log_level = args.log_level or cfg.log_level

    args.log_nonblock = args.log_nonblock or (cfg.log_nonblock or false)

    --
    -- We never allow confgure the logger in background
    -- mode since we don't know how the box will be configured
    -- later.
    ffi.C.say_logger_init(args.log, args.log_level,
                          args.log_nonblock, args.log_format, 0)

    --
    -- Update cfg vars to show them in module
    -- configuration output.
    rawset(cfg, 'log', args.log)
    rawset(cfg, 'log_level', args.log_level)
    rawset(cfg, 'log_nonblock', args.log_nonblock)
    rawset(cfg, 'log_format', args.log_format)
end

--
-- Reflect the changes made by box.cfg interface
local function box_cfg_update(box_cfg)
    rawset(cfg, 'log', box_cfg.log)
    rawset(cfg, 'log_level', box_cfg.log_level)
    rawset(cfg, 'log_nonblock', box_cfg.log_nonblock)
    rawset(cfg, 'log_format', box_cfg.log_format)
end

local compat_warning_said = false
local compat_v16 = {
    logger_pid = function()
        if not compat_warning_said then
            compat_warning_said = true
            say(S_WARN, 'logger_pid() is deprecated, please use pid() instead')
        end
        return log_pid()
    end;
}

return setmetatable({
    warn = say_closure(S_WARN);
    info = say_closure(S_INFO);
    verbose = say_closure(S_VERBOSE);
    debug = say_closure(S_DEBUG);
    error = say_closure(S_ERROR);
    rotate = log_rotate;
    pid = log_pid;
    level = log_level;
    log_format = log_format;
    cfg = cfg,
    init = init,
    --
    -- Internal API to box module, not for users,
    -- names can be changed.
    box_api = {
        set_log_level = function()
            log_level(box.cfg.log_level)
        end,
        set_log_format = function()
            log_format(box.cfg.log_format)
        end,
        cfg_update = box_cfg_update,
    }
}, {
    __index = compat_v16;
})
