#include "session_redis.hpp"

#include <string>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "def/debug_def.hpp"
#include "utilities/asio_utilities.hpp"
#include "utilities/string_utilities.hpp"
#include "context.hpp"

namespace da4qi4
{
namespace Intercepter
{

std::string const SessionOnRedis::data_name = "session-redis";

std::string make_session_id(std::string const& prefix)
{
    static boost::uuids::random_generator gen;
    boost::uuids::uuid uid = gen();
    std::stringstream ss;
    ss << prefix << uid;

    return ss.str();
}

Json SessionOnRedis::create_new_session() const
{
    std::string session_id = make_session_id(_options.prefix);

    Cookie cookie(_options.name, session_id, _options.domain, _options.path);
    cookie.SetMaxAge(_options.max_age);
    cookie.SetHttpOnly(_options.http_only);
    cookie.SetSecure(_options.secure);
    cookie.SetSameSite(_options.samesite);

    return ToJson(cookie, Json());
}

void SessionOnRedis::on_request(Context& ctx) const
{
    std::string session_id = ctx->Req().GetCookie(this->_options.name);

    if (session_id.empty())
    {
        ctx->SaveData(data_name, create_new_session());
        ctx->Pass();
        return;
    }

    if (auto redis = ctx->Redis())
    {
        redis->Command("GET", {session_id}, [ctx, this](RedisValue value)
        {
            if (value.IsError())
            {
                std::cerr << value.ToString() << std::endl;
                ctx->RenderInternalServerError();
                ctx->Stop();
                return;
            }

            Json data;

            try
            {
                if (!value.ToString().empty())
                {
                    data = Json::parse(value.ToString());
                }

                if (data.empty())
                {
                    data =  create_new_session();
                }

                ctx->SaveData(data_name, std::move(data));
                ctx->Pass();
            }
            catch (Json::parse_error const& e)
            {
                std::cerr << e.what() << std::endl; //HINT : can get more detail info from e.
                ctx->RenderInternalServerError();
                ctx->Stop();
            }
            catch (std::exception const& e)
            {
                std::cerr << e.what() << std::endl;
                ctx->RenderInternalServerError();
                ctx->Stop();
            }
        });
    }
}

void SessionOnRedis::on_response(Context& ctx) const
{
    Json node = ctx->LoadData(data_name);

    if (node.empty())
    {
        ctx->Pass();
        return;
    }

    Json data;
    Cookie cookie;

    if (!FromJson(node, cookie, data))
    {
        ctx->Pass();
        return;
    }

    ctx->Res().SetCookie(cookie);

    std::string session_timeout_s = std::to_string(cookie.GetMaxAge());
    std::string session_id = cookie.GetValue();
    size_t const indent = 4;
    std::string session_value = node.dump(indent);

    if (auto redis = ctx->Redis())
    {
        redis->Command("SETEX"
                       , {session_id, session_timeout_s, session_value}
                       , [ctx](RedisValue value)
        {
            if (value.IsError())
            {
                std::cerr << value.ToString() << std::endl;
                ctx->RenderInternalServerError();
                ctx->Stop();
            }

            ctx->Pass();
        });
    }
}

void SessionOnRedis::operator()(Context ctx, On on) const
{
    if (_options.name.empty())
    {
        ctx->Pass();
        return;
    }

    if (!Utilities::iStartsWith(ctx->Req().GetUrl().path, _options.path))
    {
        ctx->Pass();
        return;
    }

    if (!ctx->HasRedis())
    {
        ctx->Pass();
        return;
    }

    if (on == Intercepter::On::Request)
    {
        on_request(ctx);
    }
    else
    {
        on_response(ctx);
    }
}

} //namespace Intercepter
} //namespace da4qi4
