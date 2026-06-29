package com.scratchbird.r2dbc;

import io.r2dbc.spi.Connection;
import io.r2dbc.spi.ConnectionFactory;
import io.r2dbc.spi.ConnectionFactoryOptions;
import io.r2dbc.spi.Result;
import io.r2dbc.spi.Row;
import io.r2dbc.spi.Statement;
import org.reactivestreams.Publisher;

/**
 * Source-visible R2DBC SPI surface for the ScratchBird route-runner lane.
 *
 * <p>The executable route tool drives these same API families through a
 * JPype bridge when the runtime classpath is supplied. This class keeps the
 * lane package structure honest without claiming a release-ready provider.</p>
 */
public final class ScratchBirdR2dbcContractSurface {
    private ScratchBirdR2dbcContractSurface() {
    }

    public static Publisher<? extends Result> execute(Connection connection, String sql) {
        Statement statement = connection.createStatement(sql);
        return statement.execute();
    }

    public static ConnectionFactoryOptions.Builder optionsBuilder() {
        return ConnectionFactoryOptions.builder();
    }

    public interface RowConsumer {
        void accept(Row row);
    }

    public interface FactoryConsumer {
        void accept(ConnectionFactory factory);
    }
}
