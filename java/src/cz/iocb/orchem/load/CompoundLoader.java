package cz.iocb.orchem.load;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.Reader;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.zip.GZIPInputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;



public class CompoundLoader
{
    private static final int batchSize = 1000;
    private final Connection connection;
    private final String idTag;
    private final String idPrefix;


    public CompoundLoader(Connection connection, String idTag, String idPrefix)
    {
        this.connection = connection;
        this.idTag = "> <" + idTag + ">";;
        this.idPrefix = idPrefix;
    }


    private void parse(InputStream inputStream, List<Integer> oldIds) throws Exception
    {
        Reader decoder = new InputStreamReader(inputStream, "US-ASCII");
        BufferedReader reader = new BufferedReader(decoder);
        String line;

        try (PreparedStatement insertStatement = connection
                .prepareStatement("insert into compounds (id, molfile) values (?,?) "
                        + "on conflict (id) do update set molfile=EXCLUDED.molfile"))
        {
            int count = 0;

            while((line = reader.readLine()) != null)
            {
                Integer id = null;
                StringBuilder sdfBuilder = new StringBuilder();

                while(!line.startsWith(">") && !line.equals("$$$$") && line != null)
                {
                    sdfBuilder.append(line);
                    sdfBuilder.append('\n');
                    line = reader.readLine();
                }

                String sdf = sdfBuilder.toString();

                do
                {
                    if(line.compareTo("$$$$") == 0)
                        break;

                    if(line.compareTo(idTag) == 0)
                    {
                        line = reader.readLine();

                        id = Integer.parseInt(line.replaceAll("^" + idPrefix, ""));
                    }
                }
                while((line = reader.readLine()) != null);

                count++;
                oldIds.remove(id);
                insertStatement.setInt(1, id);
                insertStatement.setString(2, sdf);
                insertStatement.addBatch();

                if(count % batchSize == 0)
                    insertStatement.executeBatch();
            }

            if(count % batchSize != 0)
                insertStatement.executeBatch();
        }
    }


    public void loadDirectory(File directory, boolean removeOld) throws Exception
    {
        final File[] files = directory.listFiles();

        Arrays.sort(files, new Comparator<File>()
        {
            @Override
            public int compare(File arg0, File arg1)
            {
                return arg0.getName().compareTo(arg1.getName());
            }
        });;


        List<Integer> oldIds = new ArrayList<Integer>();

        if(removeOld)
        {
            try (Statement statement = connection.createStatement())
            {
                try (ResultSet rs = statement.executeQuery("select id from compounds"))
                {
                    while(rs.next())
                        oldIds.add(rs.getInt(1));
                }
            }
        }


        for(int i = 0; i < files.length; i++)
        {
            File file = files[i];

            if(file.getName().endsWith(".sdf.gz"))
            {
                try (InputStream gzipStream = new GZIPInputStream(new FileInputStream(file)))
                {
                    System.out.println(i + ": " + file.getName());
                    parse(gzipStream, oldIds);
                }
            }
            else if(file.getName().endsWith(".zip"))
            {
                try (ZipInputStream zipStream = new ZipInputStream(new FileInputStream(file)))
                {
                    System.out.println(i + ": " + file.getName());
                    ZipEntry entry;

                    while((entry = zipStream.getNextEntry()) != null)
                    {
                        if(entry.getName().endsWith(".sdf"))
                        {
                            System.out.println("    " + entry.getName());
                            parse(zipStream, oldIds);
                        }
                    }
                }
            }
            else if(file.getName().endsWith(".sdf"))
            {
                try (InputStream fileStream = new FileInputStream(file))
                {
                    System.out.println(i + ": " + file.getName());
                    parse(fileStream, oldIds);
                }
            }
        }


        if(removeOld)
        {
            try (PreparedStatement deleteStatement = connection.prepareStatement("delete from compounds where id = ?"))
            {
                int count = 0;

                for(int id : oldIds)
                {
                    count++;
                    deleteStatement.setInt(1, id);
                    deleteStatement.addBatch();

                    if(count % batchSize == 0)
                        deleteStatement.executeBatch();
                }

                if(count % batchSize != 0)
                    deleteStatement.executeBatch();
            }
        }
    }


    public void loadDirectory(File directory) throws Exception
    {
        loadDirectory(directory, true);
    }
}
